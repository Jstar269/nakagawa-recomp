// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later


#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include "recomp.h"
#include "dispatch_table.h"   /* guest code-address table + primitives (issue #45) */

uint8_t *g_mem = NULL;
int sr_hit_hle = 0;
int g_sr_heap_watch = 0;
extern CpuState *s_cpu;

#include <time.h>
extern uint64_t SDL_GetTicksNS(void);

#define PROF_HASH_SIZE 131072
typedef struct {
    uint32_t pc;
    uint64_t call_count;
    uint64_t block_count;
    uint64_t duration_ns;
    uint8_t used;
} SrProfEntry;

SrProfEntry g_prof_table[PROF_HASH_SIZE];
int g_prof_enabled = 0;
static uint64_t g_prof_lookup_drops = 0;

static inline SrProfEntry *prof_lookup(uint32_t pc) {
    uint32_t h = (pc * 2654435761u) & (PROF_HASH_SIZE - 1);
    for (int i = 0; i < 64; i++) {
        uint32_t idx = (h + i) & (PROF_HASH_SIZE - 1);
        if (g_prof_table[idx].used && g_prof_table[idx].pc == pc) {
            return &g_prof_table[idx];
        }
        if (!g_prof_table[idx].used) {
            g_prof_table[idx].used = 1;
            g_prof_table[idx].pc = pc;
            return &g_prof_table[idx];
        }
    }
    g_prof_lookup_drops++;
    return NULL;
}

#ifdef SR_PROFILER_SELFTEST
static SrProfEntry *prof_find(uint32_t pc) {
    uint32_t h = (pc * 2654435761u) & (PROF_HASH_SIZE - 1);
    for (int i = 0; i < 64; i++) {
        uint32_t idx = (h + i) & (PROF_HASH_SIZE - 1);
        if (!g_prof_table[idx].used) return NULL;
        if (g_prof_table[idx].pc == pc) return &g_prof_table[idx];
    }
    return NULL;
}
#endif

void sr_profile_init(void) {
    memset(g_prof_table, 0, sizeof(g_prof_table));
    g_prof_lookup_drops = 0;
    const char *e = getenv("SR_PROFILE");
    g_prof_enabled = (e != NULL && strcmp(e, "0") != 0) || ((g_sr_debug & 0x04) != 0);
    if (g_prof_enabled) {
        fprintf(stderr, "[profiler] Hot-path profiling enabled (SR_PROFILE=1 or SR_DEBUG & 0x04)\n");
        atexit(sr_profile_dump);
    }
}

void sr_profile_dump(void) {
    if (!g_prof_enabled) return;
    fprintf(stderr, "--- PERF_PROFILE ---\n");
    fprintf(stderr, "timestamp:%llu\n", (unsigned long long)time(NULL));
    for (int i = 0; i < PROF_HASH_SIZE; i++) {
        if (g_prof_table[i].used) {
            if (g_prof_table[i].call_count > 0 || g_prof_table[i].block_count > 0) {
                fprintf(stderr, "pc=0x%08x calls=%llu blocks=%llu duration_ns=%llu\n",
                        g_prof_table[i].pc,
                        (unsigned long long)g_prof_table[i].call_count,
                        (unsigned long long)g_prof_table[i].block_count,
                        (unsigned long long)g_prof_table[i].duration_ns);
            }
        }
    }
    fprintf(stderr, "lookup_drops:%llu\n", (unsigned long long)g_prof_lookup_drops);
    fprintf(stderr, "--- END_PERF_PROFILE ---\n");
    fflush(stderr);
}

void sr_profile_block(uint32_t target_pc) {
    SrProfEntry *entry = prof_lookup(target_pc);
    if (entry) {
        entry->block_count++;
    }
}

#ifdef SR_PROFILER_SELFTEST
void sr_profile_test_reset(void) {
    memset(g_prof_table, 0, sizeof(g_prof_table));
    g_prof_lookup_drops = 0;
}

uint64_t sr_profile_test_block_count(uint32_t pc) {
    SrProfEntry *entry = prof_find(pc);
    return entry ? entry->block_count : 0;
}

uint64_t sr_profile_test_lookup_drops(void) {
    return g_prof_lookup_drops;
}
#endif


/* Count-leading-zeros portable fallback. We use __builtin_clz where available
 * (GCC/Clang per the standard build), but emit a slow loop otherwise so the
 * codebase compiles cleanly under MSVC if a build-port ever wants that. */
static inline int sr_clz32(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return x ? __builtin_clz(x) : 32;
#else
    if (!x) return 32;
    int n = 0;
    while ((x & 0x80000000u) == 0u) { x <<= 1; n++; }
    return n;
#endif
}

/* Circular dispatch trace buffer — last 32 dispatches before _exit or any crash. */
#define DISPATCH_TRACE_SIZE 32
struct { uint32_t target; uint32_t pc; uint32_t ra; uint32_t uid; uint32_t used; }
    g_dtrace[DISPATCH_TRACE_SIZE];
int g_dtrace_idx = 0;

static void dump_dispatch_trace(void) {
    fprintf(stderr, "--- dispatch trace (last %d) ---\n", DISPATCH_TRACE_SIZE);
    for (int i = 0; i < DISPATCH_TRACE_SIZE; i++) {
        int idx = (g_dtrace_idx + DISPATCH_TRACE_SIZE - 1 - i) % DISPATCH_TRACE_SIZE;
        /* Skip only never-written slots. A recorded target of 0 is a real event -- a
         * null/indirect dispatch -- and must be printable, not mistaken for an empty
         * record (issue #45). */
        if (!g_dtrace[idx].used) continue;
        fprintf(stderr, "  [%2d] tgt=0x%08x pc=0x%08x ra=0x%08x uid=0x%x\n",
                i, g_dtrace[idx].target, g_dtrace[idx].pc,
                g_dtrace[idx].ra, g_dtrace[idx].uid);
    }
    fprintf(stderr, "--- end dispatch trace ---\n");
    fflush(stderr);
}

/* Out-of-range (wild guest pointer) access recorder. Bounds-safe MEM_* drops these; with
 * SR_OORLOG set, log the first distinct store addresses so a dropped game flag-write can be
 * found (a masked divergence). Reads are counted only. */
unsigned long g_oor_reads = 0, g_oor_writes = 0;
void sr_oor(uint32_t a, uint32_t v, int store) {
    static int logging = -1;
    if (logging < 0) logging = getenv("SR_OORLOG") ? 1 : 0;
    if (store) {
        g_oor_writes++;
        if (logging) {
            static uint32_t seen[64]; static int n = 0; int known = 0;
            for (int i = 0; i < n; i++) if (seen[i] == a) { known = 1; break; }
            if (!known && n < 64) { seen[n++] = a;
                fprintf(stderr, "OOR store [0x%08x] = 0x%08x\n", a, v); }
        }
    } else g_oor_reads++;
}

#define SR_VRAM_LOW  0x04000000u  /* guest VRAM/eDRAM base, below user RAM */
#define SR_RAM_SIZE  0x04000000u  /* 64 MB user RAM at 0x08000000 */

void sr_mem_init(void) {
    if (!g_mem) {
        /* Arena covers guest physical [0, 0x0c000000). RAM at 0x08000000, VRAM at 0x04000000.
         * We set g_mem to the guest RAM base (SR_RAM_BASE) so guest 0x08000000 maps to g_mem. */
        uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
        if (!arena) {
            fprintf(stderr, "sr_mem_init: out of memory\n");
            abort();
        }
        g_mem = arena + 0x08000000u;
        g_sr_heap_watch = getenv("SR_HEAP_WATCH") ? 1 : 0;
        if (g_sr_heap_watch) {
            fprintf(stderr,
                "HEAP_WATCH: tracking first writes to dynamically freed allocator headers\n");
        }
    }
}

static uint32_t g_loaded_end = 0;   /* highest guest address written by the loader */

uint32_t sr_loaded_end(void) { return g_loaded_end; }

void sr_load_segment(uint32_t vaddr, const void *data, uint32_t len) {
    sr_mem_init();
    /* Bounds-check against the 192 MB guest arena. A malformed ELF or a loader that
     * computes len from attacker-controlled fields could otherwise memcpy past the
     * end of the host allocation. The allocator reserves [0, 0x0C000000) physical;
     * after the SR_RAM_BASE offset (g_mem points at 0x08000000) the lowest legal
     * load is 0x04000000 (VRAM/eDRAM region) and the highest is just below
     * 0x0C000000. Reject anything that would write outside that space. */
    uint32_t end_vaddr = vaddr + len;
    if (len == 0u) return;
    /* u32 arithmetic overflow guard: vaddr + len must be >= vaddr for a well-formed seg */
    if (end_vaddr < vaddr) {
        fprintf(stderr, "sr_load_segment: vaddr=0x%08x len=0x%08x wraps the u32 range — refusing\n",
                vaddr, len);
        abort();
    }
    /* Validate the complete physical span, not just its two endpoints.  The
     * SR_PHYS aliasing model means independently checking start/end can accept
     * a span that crosses an alias boundary and is much larger than the host
     * allocation. */
    if (!sr_guest_span_writable(vaddr, len)) {
        fprintf(stderr, "sr_load_segment: vaddr=0x%08x len=0x%08x end=0x%08x outside guest arena — refusing\n",
                vaddr, len, end_vaddr);
        abort();
    }
    fprintf(stderr, "sr_load_segment: vaddr=0x%08x len=0x%08x SR_HOST=0x%p\n", vaddr, len, SR_HOST(vaddr));
    memcpy(SR_HOST(vaddr), data, len);
    if (vaddr + len > g_loaded_end) g_loaded_end = vaddr + len;
}

/* Newlib malloc/free backing store (guest addresses 0x00010738 / 0x000104e0 in
 * tools/codegen.py's custom stubs). Per-block header at (payload - 8):
 *   +0: while free, the guest address of the next free block (0 = list end).
 *       While allocated, unused.
 *   +4: total block size (header + payload, always a multiple of 16) with bit0 set
 *       while allocated and clear while free.
 * A long play session that streams and discards similarly-sized assets would
 * otherwise exhaust the arena via monotonic bump growth with no reuse; the free
 * list lets freed blocks satisfy later allocations of the same or smaller size. */
/* Arena placement (2026-07-16): [0x0a000008, 0x0c000000) -- 32 MB in the top of the
 * flat guest arena, above everything else: image+BSS end 0x0034c480, user partition
 * [0x0034d000, 0x0a000000), VRAM [0x04000000, 0x08000000), thread stacks 0x09exxxxx,
 * interrupt stack 0x09df0000. The previous home [0x03000008, 0x04000000) gave only
 * ~16 MB and the boot working set genuinely exceeds it (the game budgets 0x1340000 =
 * 20.25 MB for its own UserSbrk pool); before 2026-07-16 the end was 0x08000000, which
 * silently let heap blocks alias guest VRAM once the carve crossed 16 MB. SR_PHYS
 * masks to [0, 0x0c000000), so this region is fully addressable and calloc-zeroed. */
#define SR_HEAP_BASE 0x0a000008u
#define SR_HEAP_END  0x0c000000u  /* exclusive: end of the guest arena */
static uint32_t s_heap_bump_ptr = SR_HEAP_BASE;
static const uint32_t s_heap_bump_end = SR_HEAP_END;
static uint32_t s_heap_free_list = 0u;
static uint32_t s_heap_fail_count = 0u;

/* One bit per possible 16-byte block-header position.  A range check alone is
 * insufficient: an interior/foreign pointer can still land inside the arena,
 * and interpreting its payload as (next,size) poisons the free list. */
#define SR_HEAP_HEADER_SLOTS (((SR_HEAP_END - SR_HEAP_BASE) + 15u) / 16u)
static uint8_t s_heap_header_bits[(SR_HEAP_HEADER_SLOTS + 7u) / 8u];
static uint8_t s_heap_free_bits[(SR_HEAP_HEADER_SLOTS + 7u) / 8u];
static uint8_t s_heap_watch_reported_bits[(SR_HEAP_HEADER_SLOTS + 7u) / 8u];
static unsigned s_heap_watch_report_count = 0;
static int s_heap_internal_write = 0;

static int sr_heap_header_slot(uint32_t hdr, uint32_t *slot) {
    if (hdr < SR_HEAP_BASE || hdr >= SR_HEAP_END) return 0;
    uint32_t delta = hdr - SR_HEAP_BASE;
    if ((delta & 15u) != 0u) return 0;
    *slot = delta >> 4;
    return *slot < SR_HEAP_HEADER_SLOTS;
}

static void sr_heap_mark_header(uint32_t hdr) {
    uint32_t slot;
    if (sr_heap_header_slot(hdr, &slot))
        s_heap_header_bits[slot >> 3] |= (uint8_t)(1u << (slot & 7u));
}

static int sr_heap_is_header(uint32_t hdr) {
    uint32_t slot;
    return sr_heap_header_slot(hdr, &slot) &&
        (s_heap_header_bits[slot >> 3] & (uint8_t)(1u << (slot & 7u))) != 0u;
}

/* Validate one free-list node without trusting its fields for host pointer
 * arithmetic. The footer is part of the free-block contract; checking it
 * catches stale or overlapping metadata before allocation follows its link. */
static int sr_heap_free_node_valid(uint32_t hdr, uint32_t *size_out, uint32_t *next_out) {
    if (!sr_heap_is_header(hdr) || hdr < SR_HEAP_BASE || hdr >= s_heap_bump_ptr) return 0;
    uint32_t sizeflags = MEM_R32(hdr + 4u);
    uint32_t size = sizeflags & ~1u;
    if ((sizeflags & 1u) != 0u || size < 16u || (size & 15u) != 0u) return 0;
    uint32_t end = hdr + size;
    if (end < hdr || end > s_heap_bump_ptr || MEM_R32(end - 4u) != size) return 0;
    uint32_t next = MEM_R32(hdr + 0u);
    if (next != 0u && (next < SR_HEAP_BASE || next >= s_heap_bump_ptr ||
                       !sr_heap_is_header(next) || next == hdr)) return 0;
    if (size_out) *size_out = size;
    if (next_out) *next_out = next;
    return 1;
}

/* The free list is guest-visible metadata and can be damaged by a foreign
 * pointer write. Validate every reachable node and use Floyd's tortoise/hare
 * walk so a cycle cannot spin allocation forever. The interval check rejects a
 * link into another node's payload, which would otherwise create overlapping
 * free blocks even though both addresses happen to be marked headers. */
static int sr_heap_free_list_valid(void) {
    uint32_t slow = s_heap_free_list, fast = s_heap_free_list;
    while (slow != 0u || fast != 0u) {
        if (slow != 0u) {
            uint32_t slow_size, slow_next;
            if (!sr_heap_free_node_valid(slow, &slow_size, &slow_next)) return 0;
            if (slow_next != 0u) {
                uint32_t next_size;
                if (!sr_heap_free_node_valid(slow_next, &next_size, NULL)) return 0;
                uint64_t slow_end = (uint64_t)slow + slow_size;
                uint64_t next_end = (uint64_t)slow_next + next_size;
                if (((uint64_t)slow_next > slow && (uint64_t)slow_next < slow_end) ||
                    ((uint64_t)slow > slow_next && (uint64_t)slow < next_end)) return 0;
            }
            slow = slow_next;
        }
        if (fast != 0u) {
            uint32_t fast_next;
            if (!sr_heap_free_node_valid(fast, NULL, &fast_next)) return 0;
            fast = fast_next;
            if (fast != 0u) {
                if (!sr_heap_free_node_valid(fast, NULL, &fast_next)) return 0;
                fast = fast_next;
            }
        }
        if (slow != 0u && slow == fast) return 0;
    }
    return 1;
}

static void sr_heap_set_free_state(uint32_t hdr, int is_free) {
    if (!g_sr_heap_watch) return;
    uint32_t slot;
    if (!sr_heap_header_slot(hdr, &slot)) return;
    uint8_t mask = (uint8_t)(1u << (slot & 7u));
    if (is_free) {
        s_heap_free_bits[slot >> 3] |= mask;
        /* A later allocation/free cycle is a new lifetime and deserves a fresh report. */
        s_heap_watch_reported_bits[slot >> 3] &= (uint8_t)~mask;
    } else {
        s_heap_free_bits[slot >> 3] &= (uint8_t)~mask;
        s_heap_watch_reported_bits[slot >> 3] &= (uint8_t)~mask;
    }
}

static int sr_heap_is_unreported_free(uint32_t hdr, uint32_t *slot_out) {
    uint32_t slot;
    if (!sr_heap_header_slot(hdr, &slot)) return 0;
    uint8_t mask = (uint8_t)(1u << (slot & 7u));
    if ((s_heap_free_bits[slot >> 3] & mask) == 0u ||
        (s_heap_watch_reported_bits[slot >> 3] & mask) != 0u) {
        return 0;
    }
    if (slot_out) *slot_out = slot;
    return 1;
}

static void sr_heap_note_write_impl(
    uint32_t addr, uint32_t width, uint32_t value, uint32_t pc, int bulk
) {
    if (!g_sr_heap_watch || s_heap_internal_write || width == 0u ||
        s_heap_watch_report_count >= 128u) {
        return;
    }
    uint64_t start = (uint32_t)SR_PHYS(addr);
    uint64_t end = start + width;
    if (end <= SR_HEAP_BASE || start >= SR_HEAP_END) return;

    uint64_t clipped = start > SR_HEAP_BASE ? start : SR_HEAP_BASE;
    uint32_t delta = (uint32_t)(clipped - SR_HEAP_BASE);
    uint32_t hdr = SR_HEAP_BASE + ((delta >> 4) << 4);
    if ((uint64_t)hdr + 8u <= start) hdr += 16u;
    for (; (uint64_t)hdr < end && hdr < SR_HEAP_END; hdr += 16u) {
        uint32_t slot;
        if ((uint64_t)hdr + 8u <= start || !sr_heap_is_unreported_free(hdr, &slot)) continue;
        s_heap_watch_reported_bits[slot >> 3] |= (uint8_t)(1u << (slot & 7u));
        s_heap_watch_report_count++;
        uint32_t effective_pc = pc ? pc : (s_cpu ? s_cpu->pc : 0u);
        uint32_t ra = s_cpu ? s_cpu->r[31] : 0u;
        fprintf(stderr,
            "HEAP_HEADER_%sWRITE: header=0x%08x old_next=0x%08x old_size=0x%08x "
            "write_addr=0x%08x width=0x%x value=0x%08x pc=0x%08x ra=0x%08x uid=0x%x\n",
            bulk ? "BULK_" : "", hdr, MEM_R32(hdr), MEM_R32(hdr + 4u),
            addr, width, value, effective_pc, ra, sched_current_uid());
        fflush(stderr);
        return;  /* one provenance record per write operation is enough */
    }
}

void sr_heap_note_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    sr_heap_note_write_impl(addr, width, value, pc, 0);
}

void sr_heap_note_bulk_write(uint32_t addr, uint32_t width, uint32_t pc) {
    sr_heap_note_write_impl(addr, width, 0u, pc, 1);
}

typedef struct {
    uint32_t allocated_blocks, free_blocks;
    uint64_t allocated_bytes, free_bytes;
    uint32_t largest_free;
    uint32_t largest_addr[8], largest_size[8];
    int metadata_valid;
    /* When the linear walk breaks: the smashed header and the block just below it
     * (the prime overrun suspect), so a failure dump names the culprit directly. */
    uint32_t bad_hdr, bad_word0, bad_word4;
    uint32_t bad_prev_hdr, bad_prev_size;
    int bad_prev_allocated;
} SrHeapStats;

/* Walk the allocator's contiguous block headers without trusting the free-list
 * links.  This runs only on allocation failure, where a compact state summary is
 * far more useful than a bare bump pointer for distinguishing a real live-set
 * overflow from external fragmentation or damaged metadata. */
static SrHeapStats sr_heap_stats(void) {
    SrHeapStats st = {0};
    st.metadata_valid = 1;
    uint32_t prev = 0u, prev_sizeflags = 0u;
    for (uint32_t cur = SR_HEAP_BASE; cur < s_heap_bump_ptr; ) {
        uint32_t sizeflags = MEM_R32(cur + 4u);
        uint32_t size = sizeflags & ~1u;
        if (size < 16u || (size & 15u) != 0u ||
            cur + size < cur || cur + size > s_heap_bump_ptr) {
            st.metadata_valid = 0;
            st.bad_hdr = cur;
            st.bad_word0 = MEM_R32(cur + 0u);
            st.bad_word4 = sizeflags;
            st.bad_prev_hdr = prev;
            st.bad_prev_size = prev_sizeflags & ~1u;
            st.bad_prev_allocated = (int)(prev_sizeflags & 1u);
            break;
        }
        prev = cur;
        prev_sizeflags = sizeflags;
        if (sizeflags & 1u) {
            st.allocated_blocks++;
            st.allocated_bytes += size;
            for (unsigned i = 0; i < 8; ++i) {
                if (size <= st.largest_size[i]) continue;
                for (unsigned j = 7; j > i; --j) {
                    st.largest_size[j] = st.largest_size[j - 1];
                    st.largest_addr[j] = st.largest_addr[j - 1];
                }
                st.largest_size[i] = size;
                st.largest_addr[i] = cur;
                break;
            }
        } else {
            st.free_blocks++;
            st.free_bytes += size;
            if (size > st.largest_free) st.largest_free = size;
        }
        cur += size;
    }
    if (st.metadata_valid && !sr_heap_free_list_valid()) {
        st.metadata_valid = 0;
        st.bad_hdr = s_heap_free_list;
        if (s_heap_free_list != 0u && sr_heap_is_header(s_heap_free_list)) {
            st.bad_word0 = MEM_R32(s_heap_free_list + 0u);
            st.bad_word4 = MEM_R32(s_heap_free_list + 4u);
        }
    }
    return st;
}

static void sr_heap_dump_failure(uint32_t requested) {
    SrHeapStats st = sr_heap_stats();
    fprintf(stderr,
        "HEAP_STATE: request=0x%x carved=0x%x live=%u/0x%llx free=%u/0x%llx "
        "largest_free=0x%x metadata=%s\n",
        requested, s_heap_bump_ptr - SR_HEAP_BASE,
        st.allocated_blocks, (unsigned long long)st.allocated_bytes,
        st.free_blocks, (unsigned long long)st.free_bytes,
        st.largest_free, st.metadata_valid ? "valid" : "INVALID");
    if (!st.metadata_valid) {
        fprintf(stderr,
            "HEAP_SMASH: header=0x%08x words=0x%08x/0x%08x prev=0x%08x prev_size=0x%x "
            "prev_state=%s (prev payload 0x%08x..0x%08x is the prime overrun suspect)\n",
            st.bad_hdr, st.bad_word0, st.bad_word4,
            st.bad_prev_hdr, st.bad_prev_size,
            st.bad_prev_allocated ? "live" : "free",
            st.bad_prev_hdr + 8u, st.bad_prev_hdr + st.bad_prev_size);
    }
    if (getenv("SR_HEAP_DIAG")) {
        for (unsigned i = 0; i < 8 && st.largest_size[i] != 0u; ++i) {
            fprintf(stderr, "HEAP_LIVE[%u]: payload=0x%08x size=0x%x\n",
                i, st.largest_addr[i] + 8u, st.largest_size[i]);
        }
    }
}

static int sr_heap_allocation_size(uint32_t size, uint32_t *alloc_out) {
    uint64_t payload = size ? size : 1u;
    uint64_t total = (payload + 8u + 15u) & ~15ull;
    if (total > 0xffffffffull) return 0;
    *alloc_out = (uint32_t)total;
    return 1;
}

/* Clear a slot's "this is a live allocator header" bit.  Called when a block is
 * absorbed by a coalescing merge and its header becomes interior payload, so a
 * later foreign/stale pointer to that address is not mistaken for a real block. */
static void sr_heap_unmark_header(uint32_t hdr) {
    uint32_t slot;
    if (sr_heap_header_slot(hdr, &slot))
        s_heap_header_bits[slot >> 3] &= (uint8_t)~(1u << (slot & 7u));
}

/* Remove one block from the singly-linked free list.  O(list length), which
 * coalescing keeps small; used when a forward neighbor is absorbed by a merge. */
static void sr_heap_freelist_unlink(uint32_t target) {
    uint32_t prev = 0u, cur = s_heap_free_list;
    while (cur != 0u) {
        uint32_t next = MEM_R32(cur + 0u);
        if (cur == target) {
            if (prev == 0u) s_heap_free_list = next;
            else {
                s_heap_internal_write++;
                MEM_W32(prev + 0u, next);
                s_heap_internal_write--;
            }
            return;
        }
        prev = cur;
        cur = next;
    }
}

/* Publish [hdr, hdr+size) as free, coalescing with the physically adjacent free
 * neighbours (Knuth boundary tags).  Every free block carries a FOOTER -- its size
 * repeated at hdr+size-4 -- so the following block can locate a free predecessor.
 * Each candidate merge is validated three ways: the neighbour address is a live
 * allocator header (bitmap), the sizes make the blocks exactly physically
 * adjacent, and the neighbour's allocation bit is clear.  A stray footer-shaped
 * word inside an allocated neighbour's payload therefore cannot trigger a bad
 * merge.  Absorbed metadata words are zeroed so the merged block keeps free()'s
 * clean-payload guarantee.  The caller must have marked hdr and prepared its
 * payload.  Because merging keeps the free list short, the first-fit allocation
 * scan stays O(small) instead of degrading with fragmentation. */
static void sr_heap_free_block(uint32_t hdr, uint32_t size) {
    /* Forward: absorb the next physical block if it is a valid free block. */
    uint32_t next_hdr = hdr + size;
    if (next_hdr < s_heap_bump_ptr && sr_heap_is_header(next_hdr)) {
        uint32_t nsf = MEM_R32(next_hdr + 4u);
        uint32_t nsize = nsf & ~1u;
        if ((nsf & 1u) == 0u && nsize >= 16u && (nsize & 15u) == 0u &&
            next_hdr + nsize > next_hdr && next_hdr + nsize <= s_heap_bump_ptr) {
            sr_heap_freelist_unlink(next_hdr);
            sr_heap_set_free_state(next_hdr, 0);
            sr_heap_unmark_header(next_hdr);
            s_heap_internal_write++;
            MEM_W32(next_hdr + 0u, 0u);   /* old header words become interior payload */
            MEM_W32(next_hdr + 4u, 0u);
            s_heap_internal_write--;
            size += nsize;
        }
    }
    /* Backward: fold into the previous physical block if it is a valid free block,
     * located via its footer.  The predecessor is already linked, so grow it in
     * place and leave it where it sits in the list. */
    if (hdr > SR_HEAP_BASE) {
        uint32_t pfoot = MEM_R32(hdr - 4u);
        if (pfoot >= 16u && (pfoot & 15u) == 0u && pfoot <= hdr - SR_HEAP_BASE) {
            uint32_t prev_hdr = hdr - pfoot;
            if (sr_heap_is_header(prev_hdr)) {
                uint32_t psf = MEM_R32(prev_hdr + 4u);
                if ((psf & 1u) == 0u && (psf & ~1u) == pfoot && prev_hdr + pfoot == hdr) {
                    sr_heap_set_free_state(hdr, 0);
                    sr_heap_unmark_header(hdr);
                    size += pfoot;
                    s_heap_internal_write++;
                    MEM_W32(hdr - 4u, 0u);   /* predecessor's old footer -> interior */
                    MEM_W32(hdr + 0u, 0u);   /* this block's old header -> interior  */
                    MEM_W32(hdr + 4u, 0u);
                    MEM_W32(prev_hdr + 4u, size);
                    MEM_W32(prev_hdr + size - 4u, size);
                    s_heap_internal_write--;
                    return;
                }
            }
        }
    }
    /* No predecessor merge: publish [hdr, hdr+size) as a fresh free-list head. */
    s_heap_internal_write++;
    MEM_W32(hdr + 4u, size);
    MEM_W32(hdr + size - 4u, size);
    MEM_W32(hdr + 0u, s_heap_free_list);
    s_heap_internal_write--;
    s_heap_free_list = hdr;
    sr_heap_set_free_state(hdr, 1);
}

/* Insert one already-partitioned block into the host free list.  Callers write
 * and mark the header before exposing the block as free so SR_HEAP_WATCH does
 * not mistake allocator-owned metadata initialization for a guest overwrite. */
static void sr_heap_link_free_block(uint32_t hdr, uint32_t size, int clear_payload) {
    sr_heap_mark_header(hdr);
    if (clear_payload) {
        for (uint32_t off = 8u; off < size; off += 4u) MEM_W32(hdr + off, 0u);
    }
    sr_heap_free_block(hdr, size);
}

uint32_t sr_newlib_malloc(uint32_t size, uint32_t guest_ra) {
    static int trace_on = -1;
    if (trace_on < 0) trace_on = getenv("SR_ALLOC_TRACE") ? 1 : 0;
    if (size == 0u) size = 1u;
    uint32_t alloc;
    if (!sr_heap_allocation_size(size, &alloc)) return 0u;

    /* Reuse defaults ON (see sr_newlib_free below for the root cause that used to
     * gate it off and the fix that made it safe: an arena-membership check on the
     * pointer passed to free). Set SR_HEAP_REUSE_OFF to opt back out for A/B
     * comparison against the old bump-only behavior. */
    static int reuse_on = -1;
    if (reuse_on < 0) reuse_on = getenv("SR_HEAP_REUSE_OFF") ? 0 : 1;

    if (reuse_on && s_heap_free_list != 0u && !sr_heap_free_list_valid()) {
        static int corrupt_list_n = 0;
        if (corrupt_list_n++ < 8)
            fprintf(stderr,
                "HEAP_FREE_LIST_CORRUPT: quarantining invalid/cyclic list head=0x%08x\n",
                s_heap_free_list);
        s_heap_free_list = 0u;
    }

    uint32_t prev = 0u, cur = reuse_on ? s_heap_free_list : 0u;
    while (cur != 0u) {
        if (!sr_heap_is_header(cur) || cur >= s_heap_bump_ptr) {
            static int corrupt_list_n = 0;
            if (corrupt_list_n++ < 8)
                fprintf(stderr, "HEAP_FREE_LIST_CORRUPT: header=0x%08x; quarantining list\n", cur);
            s_heap_free_list = 0u;
            break;
        }
        uint32_t block_size = MEM_R32(cur + 4u);
        uint32_t next = MEM_R32(cur + 0u);
        if ((block_size & 1u) != 0u || block_size < 16u || (block_size & 15u) != 0u ||
            cur + block_size < cur || cur + block_size > s_heap_bump_ptr ||
            (next != 0u && (!sr_heap_is_header(next) || next >= s_heap_bump_ptr))) {
            static int corrupt_block_n = 0;
            if (corrupt_block_n++ < 8)
                fprintf(stderr,
                    "HEAP_FREE_LIST_CORRUPT: header=0x%08x size=0x%x next=0x%08x; quarantining list\n",
                    cur, block_size, next);
            s_heap_free_list = 0u;
            break;
        }
        if (block_size >= alloc) {
            sr_heap_set_free_state(cur, 0);
            if (prev == 0u) s_heap_free_list = next;
            else {
                s_heap_internal_write++;
                MEM_W32(prev + 0u, next);
                s_heap_internal_write--;
            }

            /* The boundary-tag footer at cur+block_size-4 exists only while the
             * block is free; from here it is ordinary guest payload. Clear it at
             * the free->allocated transition. On the split path below the
             * remainder writes a fresh footer over this same word, but on an exact
             * fit (remainder 0) nothing else would, and the guest's allocation
             * would end with the block size instead of zero. Every allocation and
             * block size is 16-aligned, so remainder is 0 or >= 16 -- exact fit is
             * the only case that reaches the guest, but clearing unconditionally
             * keeps the invariant independent of that arithmetic. */
            s_heap_internal_write++;
            MEM_W32(cur + block_size - 4u, 0u);
            s_heap_internal_write--;

            uint32_t remainder = block_size - alloc;
            if (remainder >= 16u) {
                MEM_W32(cur + 4u, alloc | 1u);
                uint32_t new_block = cur + alloc;
                sr_heap_mark_header(new_block);
                /* Coalesces the split tail with a following free block if any. */
                sr_heap_free_block(new_block, remainder);
            } else {
                MEM_W32(cur + 4u, block_size | 1u);
            }
            uint32_t result = cur + 8u;
            if (trace_on) fprintf(stderr,
                "ALLOC_RET: size=0x%x ptr=0x%08x source=reuse block=0x%x "
                "callsite=0x%08x ra=0x%08x uid=0x%x\n",
                size, result, MEM_R32(cur + 4u) & ~1u,
                guest_ra >= 8u ? guest_ra - 8u : 0u, guest_ra, sched_current_uid());
            return result;
        }
        prev = cur;
        cur = next;
    }

    if (size >= s_heap_bump_end - SR_HEAP_BASE || s_heap_bump_ptr + alloc > s_heap_bump_end ||
        s_heap_bump_ptr + alloc < s_heap_bump_ptr) {
        if (s_heap_fail_count++ <= 10) {
            fprintf(stderr,
                "HEAP_ALLOC: fail (NULL) size=0x%x bump_ptr=0x%08x free_list=0x%08x\n",
                size, s_heap_bump_ptr, s_heap_free_list);
            sr_heap_dump_failure(size);
        }
        return 0u;
    }
    uint32_t ptr = s_heap_bump_ptr;
    s_heap_bump_ptr += alloc;
    sr_heap_mark_header(ptr);
    MEM_W32(ptr + 0u, 0u);
    MEM_W32(ptr + 4u, alloc | 1u);
    uint32_t result = ptr + 8u;
    if (trace_on) fprintf(stderr,
        "ALLOC_RET: size=0x%x ptr=0x%08x source=bump block=0x%x "
        "callsite=0x%08x ra=0x%08x uid=0x%x\n",
        size, result, alloc,
        guest_ra >= 8u ? guest_ra - 8u : 0u, guest_ra, sched_current_uid());
    return result;
}

void sr_newlib_free(uint32_t ptr, uint32_t guest_ra) {
    if (ptr == 0u) return;
    static int reuse_on = -1;
    if (reuse_on < 0) reuse_on = getenv("SR_HEAP_REUSE_OFF") ? 0 : 1;
    if (!reuse_on) return;   /* matches the arena's original no-op-free behavior exactly */
    uint32_t hdr = ptr - 8u;
    /* Arena-membership check: reject any pointer this allocator never handed out.
     * ROOT CAUSE (previously misdiagnosed as an "INIT_ARRAY/constructor ordering"
     * race -- that theory was checked and refuted, nothing at runtime legitimately
     * writes 0x3070c0 except this function): some guest free() call path was
     * passing a foreign/non-heap pointer that happened to land near 0x3070c0, a
     * static C++ vtable region baked into the image as link-time rodata. Without
     * this check, the zero-fill loop below trusted that address's in-place
     * "size|flags" word and wiped out the vtable, and every subsequent object
     * whose vptr pointed into it null-faulted on dispatch forever. Confirmed via
     * live A/B repro: baseline (reuse off) healthy for 65s; reuse on without this
     * check produced NULL_CALLs within seconds. Bounding hdr to [SR_HEAP_BASE,
     * s_heap_bump_ptr) -- the range this allocator has actually carved out --
     * makes foreign frees a safe no-op instead of arena corruption. */
    if (hdr < SR_HEAP_BASE || hdr >= s_heap_bump_ptr || !sr_heap_is_header(hdr)) {
        /* NOT rare in practice -- live testing shows this path firing tens of
         * thousands of times per run (mostly small non-pointer values like 1,
         * 0xff, 0xcf that were never real heap allocations), so it's gated behind
         * SR_HEAP_DIAG rather than logged unconditionally. The generated allocator
         * bridge now forwards the guest return address, which makes the rejecting
         * free call site and owning thread visible without changing free semantics. */
        static int diag_on = -1;
        if (diag_on < 0) diag_on = getenv("SR_HEAP_DIAG") ? 1 : 0;
        if (diag_on) fprintf(stderr,
            "ALLOC_FREE_REJECT: foreign/interior ptr=0x%08x hdr=0x%08x "
            "(not an owned block in [0x%08x,0x%08x)) "
            "callsite=0x%08x ra=0x%08x uid=0x%x\n",
            ptr, hdr, SR_HEAP_BASE, s_heap_bump_ptr,
            guest_ra >= 8u ? guest_ra - 8u : 0u, guest_ra, sched_current_uid());
        return;
    }
    uint32_t sizeflags = MEM_R32(hdr + 4u);
    if ((sizeflags & 1u) == 0u) return;   /* double-free or foreign pointer: ignore, don't corrupt the list */
    {
        static int trace_on = -1;
        if (trace_on < 0) trace_on = getenv("SR_ALLOC_TRACE") ? 1 : 0;
        if (trace_on) fprintf(stderr,
            "ALLOC_FREE: ptr=0x%08x hdr=0x%08x block=0x%x "
            "callsite=0x%08x ra=0x%08x uid=0x%x\n",
            ptr, hdr, sizeflags & ~1u,
            guest_ra >= 8u ? guest_ra - 8u : 0u, guest_ra, sched_current_uid());
    }
    /* Zero the payload so a later reuse starts clean instead of exposing this
     * block's previous occupant's leftover bytes -- confirmed by A/B test to be
     * the actual source of the corruption reuse otherwise exposes (something
     * downstream reads a reused block's stale content before fully overwriting
     * it). Real malloc doesn't guarantee zeroed memory, but that guarantee costs
     * nothing meaningful here and eliminates the corruption outright. */
    {
        uint32_t size = sizeflags & ~1u;
        for (uint32_t off = 8u; off < size; off += 4u) MEM_W32(hdr + off, 0u);
    }
    sr_heap_free_block(hdr, sizeflags & ~1u);
}

uint32_t sr_newlib_memalign(uint32_t alignment, uint32_t size, uint32_t guest_ra) {
    /* The retail newlib path delegates alignments no stricter than malloc's
     * native 16-byte payload alignment directly to _malloc_r. */
    if (alignment <= 16u) return sr_newlib_malloc(size, guest_ra);
    if ((alignment & (alignment - 1u)) != 0u) return 0u;

    uint64_t raw_request = (uint64_t)size + alignment;
    if (raw_request > 0xffffffffull) return 0u;
    uint32_t raw_payload = sr_newlib_malloc((uint32_t)raw_request, guest_ra);
    if (raw_payload == 0u) return 0u;

    uint32_t raw_hdr = raw_payload - 8u;
    uint32_t raw_total = MEM_R32(raw_hdr + 4u) & ~1u;
    uint64_t aligned_payload64 =
        ((uint64_t)raw_payload + alignment - 1u) & ~((uint64_t)alignment - 1u);
    uint32_t wanted_total;
    if (!sr_heap_allocation_size(size, &wanted_total) ||
        aligned_payload64 > 0xffffffffull) {
        sr_newlib_free(raw_payload, guest_ra);
        return 0u;
    }

    uint32_t aligned_payload = (uint32_t)aligned_payload64;
    uint32_t aligned_hdr = aligned_payload - 8u;
    uint32_t leading = aligned_hdr - raw_hdr;
    uint64_t alloc_end64 = (uint64_t)aligned_hdr + wanted_total;
    uint64_t raw_end64 = (uint64_t)raw_hdr + raw_total;
    if ((leading != 0u && leading < 16u) || alloc_end64 > raw_end64) {
        sr_newlib_free(raw_payload, guest_ra);
        return 0u;
    }

    uint32_t trailing = (uint32_t)(raw_end64 - alloc_end64);
    sr_heap_mark_header(aligned_hdr);
    MEM_W32(aligned_hdr + 0u, 0u);
    MEM_W32(aligned_hdr + 4u, wanted_total | 1u);
    sr_heap_set_free_state(aligned_hdr, 0);

    if (leading != 0u) sr_heap_link_free_block(raw_hdr, leading, 1);
    if (trailing != 0u)
        sr_heap_link_free_block(aligned_hdr + wanted_total, trailing, 1);
    return aligned_payload;
}

uint32_t sr_newlib_realloc(uint32_t ptr, uint32_t size, uint32_t guest_ra) {
    if (ptr == 0u) return sr_newlib_malloc(size, guest_ra);
    if (size == 0u) {
        sr_newlib_free(ptr, guest_ra);
        return 0u;
    }

    uint32_t hdr = ptr - 8u;
    if (hdr < SR_HEAP_BASE || hdr >= s_heap_bump_ptr || !sr_heap_is_header(hdr))
        return 0u;
    uint32_t sizeflags = MEM_R32(hdr + 4u);
    uint32_t old_total = sizeflags & ~1u;
    if ((sizeflags & 1u) == 0u || old_total < 16u || (old_total & 15u) != 0u ||
        hdr + old_total < hdr || hdr + old_total > s_heap_bump_ptr) {
        return 0u;
    }

    uint32_t wanted_total;
    if (!sr_heap_allocation_size(size, &wanted_total)) return 0u;
    if (wanted_total <= old_total) {
        uint32_t trailing = old_total - wanted_total;
        if (trailing >= 16u) {
            MEM_W32(hdr + 4u, wanted_total | 1u);
            sr_heap_link_free_block(hdr + wanted_total, trailing, 1);
        }
        return ptr;
    }

    /* Check if the physically adjacent successor block is free and offers enough space to grow in-place. */
    uint32_t next_hdr = hdr + old_total;
    if (next_hdr < s_heap_bump_ptr && sr_heap_is_header(next_hdr)) {
        uint32_t nsf = MEM_R32(next_hdr + 4u);
        uint32_t nsize = nsf & ~1u;
        if ((nsf & 1u) == 0u && nsize >= 16u && (nsize & 15u) == 0u &&
            next_hdr + nsize > next_hdr && next_hdr + nsize <= s_heap_bump_ptr) {
            uint32_t combined = old_total + nsize;
            if (combined >= wanted_total) {
                sr_heap_freelist_unlink(next_hdr);
                sr_heap_set_free_state(next_hdr, 0);
                sr_heap_unmark_header(next_hdr);
                s_heap_internal_write++;
                MEM_W32(next_hdr + 0u, 0u);
                MEM_W32(next_hdr + 4u, 0u);
                s_heap_internal_write--;

                uint32_t trailing = combined - wanted_total;
                if (trailing >= 16u) {
                    MEM_W32(hdr + 4u, wanted_total | 1u);
                    sr_heap_link_free_block(hdr + wanted_total, trailing, 1);
                } else {
                    MEM_W32(hdr + 4u, combined | 1u);
                }
                return ptr;
            }
        }
    }

    uint32_t replacement = sr_newlib_malloc(size, guest_ra);
    if (replacement == 0u) return 0u;  /* realloc leaves the old allocation intact */
    uint32_t copy_size = old_total - 8u;
    if (copy_size > size) copy_size = size;
    uint32_t off = 0u;
    for (; off + 4u <= copy_size; off += 4u)
        MEM_W32(replacement + off, MEM_R32(ptr + off));
    for (; off < copy_size; ++off)
        MEM_W8(replacement + off, MEM_R8(ptr + off));
    sr_newlib_free(ptr, guest_ra);
    return replacement;
}

/* Unaligned word access. Little-endian merge, identical to PPSSPP's interpreter. */
uint32_t sr_lwl(uint32_t rtv, uint32_t addr) {
    uint32_t shift = (addr & 3) * 8;
    uint32_t mem = sr_r32(addr & ~3u);
    return (rtv & (0x00ffffffu >> shift)) | (mem << (24 - shift));
}
uint32_t sr_lwr(uint32_t rtv, uint32_t addr) {
    uint32_t shift = (addr & 3) * 8;
    uint32_t mem = sr_r32(addr & ~3u);
    return (rtv & (0xffffff00u << (24 - shift))) | (mem >> shift);
}
void sr_swl_pc(uint32_t addr, uint32_t rtv, uint32_t pc) {
    uint32_t shift = (addr & 3) * 8;
    uint32_t mem = sr_r32(addr & ~3u);
    sr_w32_pc(addr & ~3u, (rtv >> (24 - shift)) | (mem & (0xffffff00u << shift)), pc);
}
void sr_swr_pc(uint32_t addr, uint32_t rtv, uint32_t pc) {
    uint32_t shift = (addr & 3) * 8;
    uint32_t mem = sr_r32(addr & ~3u);
    sr_w32_pc(addr & ~3u, (rtv << shift) | (mem & (0x00ffffffu >> (24 - shift))), pc);
}
void sr_swl(uint32_t addr, uint32_t rtv) { sr_swl_pc(addr, rtv, 0u); }
void sr_swr(uint32_t addr, uint32_t rtv) { sr_swr_pc(addr, rtv, 0u); }

void sr_break(CpuState *s, uint32_t code, uint32_t pc) {
    (void)s;
    /* PSP uses BREAK for assertions; log/report and continue, or abort if fatal.
     * This is a controlled compatibility approximation, not full PSP BREAK exception emulation. */
    fprintf(stderr, "[recomp/cpu] BREAK 0x%x encountered at pc=0x%08x\n", code, pc);
    const char *fatal = getenv("SR_BREAK_FATAL");
    if (fatal && fatal[0] != '\0' && strcmp(fatal, "0") != 0) {
        fprintf(stderr, "Fatal error: SR_BREAK_FATAL is set; aborting.\n");
        abort();
    }
}

#ifndef SR_GATE_BUILD
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc) {
    (void)s;
    /* Production behavior: unsupported raw MIPS syscall is explicit/fatal/diagnostic. */
    fprintf(stderr, "Fatal error: unsupported raw MIPS syscall 0x%x at pc=0x%08x\n", code, pc);
    abort();
}
#endif


uint32_t sr_bitrev(uint32_t x) {
    x = ((x >> 1) & 0x55555555) | ((x << 1) & 0xaaaaaaaa);
    x = ((x >> 2) & 0x33333333) | ((x << 2) & 0xcccccccc);
    x = ((x >> 4) & 0x0f0f0f0f) | ((x << 4) & 0xf0f0f0f0);
    x = ((x >> 8) & 0x00ff00ff) | ((x << 8) & 0xff00ff00);
    return (x >> 16) | (x << 16);
}

/* ---- VFPU transcendental kernels (exact ports of PPSSPP's table-based kernels) ---- */

#include <math.h>

/* The lookup tables are owned by vfpu_tables.c: loaded once with exact-length,
 * EOF, SHA-256 and value-domain validation, then published atomically (#187). */
#include "vfpu_tables.h"

static uint32_t sr_exp2_approx(uint32_t x) {
    if (x == 0x00800000u) return 0x00800000u;
    uint32_t a = sr_exp2_lut65536[x >> 16];
    x &= 0x0000FFFFu;
    uint32_t b = (uint32_t)(((2977151143ull * x) >> 23) + ((1032119999ull * (x * x)) >> 46));
    return (a + (uint32_t)(((uint64_t)(a + (1u << 23)) * (uint64_t)b) >> 32)) & 0xFFFFFFFCu;
}

static uint32_t sr_exp2_fixed(uint32_t x) {
    if (x == 0u) return 0u;
    if (x == 0x00800000u) return 0x00800000u;
    uint32_t A = sr_exp2_approx(x & 0xFFFFFFC0u);
    uint32_t B = sr_exp2_approx((x + 64) & 0xFFFFFFC0u);
    uint64_t a = ((uint64_t)A << 4) + (uint64_t)sr_exp2_lut[x >> 6][0] - 64u;
    uint64_t b = ((uint64_t)B << 4) + (uint64_t)sr_exp2_lut[x >> 6][1] - 64u;
    uint32_t y = (uint32_t)((a + (((b - a) * (x & 63)) >> 6)) >> 4);
    y &= 0xFFFFFFFCu;
    return y;
}

float sr_vfpu_exp2(float x) {
    sr_vfpu_load();
    int32_t bits;
    memcpy(&bits, &x, 4);
    if ((bits & 0x7FFFFFFF) <= 0x007FFFFF) return 1.0f;
    if (x != x) { bits = 0x7F800001; memcpy(&x, &bits, 4); return x; }
    if (x <= -126.0f) return 0.0f;
    if (x >= 128.0f) { bits = 0x7F800000; memcpy(&x, &bits, 4); return x; }
    /* Everything from here on is positive finite; cast through uint32_t for
     * the bitwise masks below (the original int32_t arithmetic with &= was UB). */
    uint32_t mant_q23 = (uint32_t)((int32_t)((float)(int32_t)(x * 8388608.0f)));
    if (x < 0.0f) mant_q23 -= 1u;
    uint32_t exp_part  = (uint32_t)(0x3F800000u + (mant_q23 & 0xFF800000u) + sr_exp2_fixed(mant_q23 & 0x007FFFFFu));
    bits = (int32_t)exp_part;
    memcpy(&x, &bits, 4);
    return x;
}

static uint32_t sr_sin_quantum(uint32_t x) {
    return x < (1u << 22) ? 1u : 1u << (32 - 22 - sr_clz32(x));
}
static uint32_t sr_sin_truncate_bits(uint32_t x) {
    return x & (0u - sr_sin_quantum(x));
}

static uint32_t sr_sin_fixed(uint32_t arg) {
    if (arg == 0u) return 0u;
    if (arg == 0x00800000u) return 0x10000000u;
    uint32_t L = sr_sin_lut8192[(arg >> 13) + 0];
    uint32_t H = sr_sin_lut8192[(arg >> 13) + 1];
    uint32_t A = L + (((H - L) * (((arg >> 6) & 127) + 0)) >> 7);
    uint32_t B = L + (((H - L) * (((arg >> 6) & 127) + 1)) >> 7);
    uint64_t a = ((uint64_t)A << 5) +
        (uint64_t)(int64_t)sr_sin_lut_delta[arg >> 6][0] * sr_sin_quantum(A);
    uint64_t b = ((uint64_t)B << 5) +
        (uint64_t)(int64_t)sr_sin_lut_delta[arg >> 6][1] * sr_sin_quantum(B);
    uint32_t v = (uint32_t)(((a * (64 - (arg & 63)) + b * (arg & 63)) >> 6) >> 5);
    v = sr_sin_truncate_bits(v);
    /* The exception table is a compact sorted set, not a direct lookup.  Its interval
     * bounds are reconstructed from a linear estimate plus signed table deltas. */
    uint32_t lo = ((169u * ((arg >> 7) + 0)) >> 7) +
        (uint32_t)(int32_t)sr_sin_lut_interval_delta[(arg >> 7) + 0] + 16384u;
    uint32_t hi = ((169u * ((arg >> 7) + 1)) >> 7) +
        (uint32_t)(int32_t)sr_sin_lut_interval_delta[(arg >> 7) + 1] + 16384u;
    /* Defense in depth: validated deltas keep lo/hi <= exceptions length, so
     * every m = (lo+hi)/2 < hi stays in bounds; clamp hi to keep the search
     * read safe even if a corrupt table slips through (#187). */
    if (hi > SR_VFPU_SIN_EXCEPTIONS_BYTES) hi = SR_VFPU_SIN_EXCEPTIONS_BYTES;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        uint32_t b8 = sr_sin_lut_exceptions[m];
        uint32_t e = (arg & ~127u) + (b8 & 127u);
        if (e == arg) {
            v += sr_sin_quantum(v) * (b8 >> 7 ? UINT32_MAX : 1u);
            break;
        }
        if (e < arg) lo = m + 1;
        else hi = m;
    }
    return v;
}

float sr_vfpu_sin(float x) {
    sr_vfpu_load();
    uint32_t bits; memcpy(&bits, &x, 4);
    uint32_t sign=bits&0x80000000u, exponent=(bits>>23)&0xFFu;
    uint32_t significand=(bits&0x007FFFFFu)|0x00800000u;
    if(exponent==0xFFu){bits=sign^0x7F800001u;memcpy(&x,&bits,4);return x;}
    if(exponent<0x7Fu){
        if(exponent<0x7Fu-23u)significand=0u;else significand>>=(0x7F-exponent);
    }else if(exponent>0x7Fu){
        if(exponent-0x7Fu>=25u&&exponent-0x7Fu<32u)significand=0u;
        else if((exponent&0x9Fu)==0x9Fu)significand=0u;
        else significand<<=((exponent-0x7Fu)&31);
    }
    sign^=(significand<<7)&0x80000000u;
    significand&=0x00FFFFFFu;
    if(significand>0x00800000u)significand=0x01000000u-significand;
    float out=(float)(int32_t)sr_sin_fixed(significand)*3.7252902984619140625e-9f;
    return sign?-out:out;
}

float sr_vfpu_cos(float x) {
    sr_vfpu_load();
    uint32_t bits;memcpy(&bits,&x,4);bits&=0x7FFFFFFFu;
    uint32_t sign=0,exponent=(bits>>23)&0xFFu;
    uint32_t significand=(bits&0x007FFFFFu)|0x00800000u;
    if(exponent==0xFFu){bits=0x7F800001u;memcpy(&x,&bits,4);return x;}
    if(exponent<0x7Fu){
        if(exponent<0x7Fu-23u)significand=0u;else significand>>=(0x7F-exponent);
    }else if(exponent>0x7Fu){
        if(exponent-0x7Fu>=25u&&exponent-0x7Fu<32u)significand=0u;
        else if((exponent&0x9Fu)==0x9Fu)significand=0u;
        else significand<<=((exponent-0x7Fu)&31);
    }
    sign^=(significand<<7)&0x80000000u;
    significand&=0x00FFFFFFu;
    if(significand>=0x00800000u){significand=0x01000000u-significand;sign^=0x80000000u;}
    float out=(float)(int32_t)sr_sin_fixed(0x00800000u-significand)*3.7252902984619140625e-9f;
    return sign?-out:out;
}

static uint32_t sr_rcp_approx(uint32_t i){
    return 0x3E800000u+((uint32_t)((1ull<<47)/((1ull<<23)+i))&~3u);
}

float sr_vfpu_rcp(float x) {
    sr_vfpu_load();
    uint32_t bits;memcpy(&bits,&x,4);
    uint32_t s=bits&0x80000000u,e=bits&0x7F800000u,i=bits&0x007FFFFFu;
    if((bits&0x7FFFFFFFu)>0x7E800000u){
        bits=(e==0x7F800000u&&i?s^0x7F800001u:s);memcpy(&x,&bits,4);return x;
    }
    if(e==0u){bits=s^0x7F800000u;memcpy(&x,&bits,4);return x;}
    uint32_t A=sr_rcp_approx(i&~63u),B=sr_rcp_approx((i+64)&~63u);
    uint64_t a=((uint64_t)A<<6)+(uint64_t)(int64_t)sr_rcp_lut[i>>6][0]*4u;
    uint64_t b=((uint64_t)B<<6)+(uint64_t)(int64_t)sr_rcp_lut[i>>6][1]*4u;
    uint32_t v=(uint32_t)((a+(((b-a)*(i&63))>>6))>>6)&~3u;
    bits=s+(0x3F800000u-e)+v;memcpy(&x,&bits,4);return x;
}

static uint32_t sr_isqrt23(uint32_t x){
    uint64_t t=(uint64_t)x<<23,m=0x4000000000000000ull,y=0;
    while(m){uint64_t b=y|m;y>>=1;if(t>=b){t-=b;y|=m;}m>>=2;}return (uint32_t)y;
}
static uint32_t sr_sqrt_fixed(uint32_t x){
    uint32_t lo=(x+0)&~63u,hi=(x+64)&~63u;
    lo=lo>=0x00400000u?4u*lo:0x00800000u+2u*lo;
    hi=hi>=0x00400000u?4u*hi:0x00800000u+2u*hi;
    uint32_t A=0x3F000000u+sr_isqrt23(lo),B=0x3F000000u+sr_isqrt23(hi);
    uint64_t a=((uint64_t)A<<4)+(uint64_t)(int64_t)sr_sqrt_lut[x>>6][0];
    uint64_t b=((uint64_t)B<<4)+(uint64_t)(int64_t)sr_sqrt_lut[x>>6][1];
    return ((uint32_t)((a+(((b-a)*(x&63))>>6))>>4))&~3u;
}
float sr_vfpu_sqrt(float x){
    sr_vfpu_load();uint32_t bits;memcpy(&bits,&x,4);
    if((bits&0x7FFFFFFFu)<=0x007FFFFFu)return 0.0f;
    if(bits>>31){bits=0x7F800001u;memcpy(&x,&bits,4);return x;}
    if((bits>>23)==255u){bits=0x7F800000u+((bits&0x007FFFFFu)!=0u);memcpy(&x,&bits,4);return x;}
    int32_t exponent=(int32_t)(bits>>23)-127;
    uint32_t index=((bits+0x00800000u)>>1)&0x007FFFFFu;
    bits=sr_sqrt_fixed(index);bits+=(uint32_t)(exponent>>1)<<23;
    memcpy(&x,&bits,4);return x;
}
static uint32_t sr_rsqrt_floor22(uint32_t x){
    uint64_t t=(uint64_t)x<<22,m=0x4000000000000000ull,y=0;
    while(m){uint64_t b=y|m;y>>=1;if(t>=b){t-=b;y|=m;}m>>=2;}
    y=(1ull<<44)/y;
    if(((y*y)>>3)*x>(1ull<<63)-3ull*(((y&7)==6)<<21))--y;
    return (uint32_t)y;
}
static uint32_t sr_rsqrt_fixed(uint32_t x){
    uint32_t lo=(x+0)&~63u,hi=(x+64)&~63u;
    lo=lo>=0x00400000u?2u*lo:0x00400000u+lo;
    hi=hi>=0x00400000u?2u*hi:0x00400000u+hi;
    uint32_t A=0x3E800000u+4u*sr_rsqrt_floor22(lo),B=0x3E800000u+4u*sr_rsqrt_floor22(hi);
    uint64_t a=((uint64_t)A<<4)+(uint64_t)(int64_t)sr_rsqrt_lut[x>>6][0];
    uint64_t b=((uint64_t)B<<4)+(uint64_t)(int64_t)sr_rsqrt_lut[x>>6][1];
    return ((uint32_t)((a+(((b-a)*(x&63))>>6))>>4))&~3u;
}
float sr_vfpu_rsqrt(float x){
    sr_vfpu_load();uint32_t bits;memcpy(&bits,&x,4);
    if((bits&0x7FFFFFFFu)<=0x007FFFFFu){bits=0x7F800000u|(bits&0x80000000u);memcpy(&x,&bits,4);return x;}
    if(bits>>31){bits=0xFF800001u;memcpy(&x,&bits,4);return x;}
    if((bits>>23)==255u){bits=(bits&0x007FFFFFu)?0x7F800001u:0u;memcpy(&x,&bits,4);return x;}
    int32_t exponent=(int32_t)(bits>>23)-127;
    uint32_t index=((bits+0x00800000u)>>1)&0x007FFFFFu;
    bits=sr_rsqrt_fixed(index);bits-=(uint32_t)(exponent>>1)<<23;
    memcpy(&x,&bits,4);return x;
}

static uint32_t sr_asin_quantum(uint32_t x){return x<(1u<<23)?1u:1u<<(32-23-sr_clz32(x));}
static uint32_t sr_asin_truncate(uint32_t x){return x&(0u-sr_asin_quantum(x));}
static uint32_t sr_asin_approx(uint32_t x){
    const int32_t *C=sr_asin_lut65536[x>>16];x&=0xFFFFu;
    return sr_asin_truncate((uint32_t)((((((int64_t)C[2]*x)>>16)+C[1])*x>>16)+C[0]));
}
static uint32_t sr_asin_fixed(uint32_t x){
    if(x==0)return 0;
    if(x==(1u<<23))return 1u<<30;
    uint32_t ret=sr_asin_approx(x),index=sr_asin_lut_indices[x/21u];
    /* Defense in depth: validated tables bound every index below the deltas
     * entry count; a corrupt value must not become an OOB read (#187). */
    if(index>=SR_VFPU_ASIN_DELTAS_ENTRIES)index=0;
    uint64_t deltas=sr_asin_lut_deltas[index];
    return ret+(3u-(uint32_t)((deltas>>(3u*(x%21u)))&7u))*sr_asin_quantum(ret);
}
float sr_vfpu_asin(float x){
    sr_vfpu_load();uint32_t bits;memcpy(&bits,&x,4);uint32_t sign=bits&0x80000000u;
    bits&=0x7FFFFFFFu;if(bits>0x3F800000u){bits=0x7F800001u^sign;memcpy(&x,&bits,4);return x;}
    memcpy(&x,&bits,4);bits=sr_asin_fixed((uint32_t)(int32_t)(x*8388608.0f));
    x=(float)(int32_t)bits*9.31322574615478515625e-10f;return sign?-x:x;
}

static uint32_t sr_log2_approx(uint32_t x){
    uint32_t a=sr_log2_lut65536[(x>>16)+0],b=sr_log2_lut65536[(x>>16)+1];
    uint32_t c=sr_log2_lut65536_quadratic[x>>16];x&=0xFFFFu;
    uint64_t ret=(uint64_t)a*(0x10000u-x)+(uint64_t)b*x;
    ret+=((uint64_t)c*x*(0x10000u-x))>>40;return (uint32_t)(ret>>16);
}
float sr_vfpu_log2(float x){
    sr_vfpu_load();uint32_t bits;memcpy(&bits,&x,4);
    if((bits&0x7FFFFFFFu)<=0x007FFFFFu){bits=0xFF800000u;memcpy(&x,&bits,4);return x;}
    if(bits&0x80000000u){bits=0x7F800001u;memcpy(&x,&bits,4);return x;}
    if((bits>>23)==255u){bits=0x7F800000u+((bits&0x007FFFFFu)!=0u);memcpy(&x,&bits,4);return x;}
    uint32_t e=(bits&0x7F800000u)-0x3F800000u,i=bits&0x007FFFFFu;
    if((e>>31)&&i>=0x007FFE00u){
        float c=(float)((int32_t)(~e)>>23);return i<0x007FFEF7u?-3.05175781e-05f-c:-0.0f-c;
    }
    int d=e<0x01000000u?0:8-sr_clz32(e)-(int)(e>>31);uint32_t q=1u<<d;
    uint32_t A=sr_log2_approx(i&~63u)&(0u-q),B=sr_log2_approx((i+64)&~63u)&(0u-q);
    uint64_t a=((uint64_t)A<<6)+((uint64_t)sr_log2_lut[d][i>>6][0]-80ull)*q;
    uint64_t b=((uint64_t)B<<6)+((uint64_t)sr_log2_lut[d][i>>6][1]-80ull)*q;
    uint32_t v=(uint32_t)((a+(((b-a)*(i&63))>>6))>>6);v&=0u-q;
    bits=e^(2u*v);return (float)(int32_t)bits*1.1920928955078125e-7f;
}

/* ---- VFPU prefix application (ports of PPSSPP ApplyPrefixST / ApplyPrefixD) ---- */

static const float SR_VFPU_CONST[8] = {0.0f, 1.0f, 2.0f, 0.5f, 3.0f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f};

void sr_vread(float *r, const CpuState *s, const uint8_t *idx, int n, uint32_t prefix) {
    if (n > 4) n = 4;  /* vector lanes are at most 4; the codegen always passes <=4 */
    /* Initial lane load */
    for (int i = 0; i < n; i++)
        r[i] = s->v[idx[i]];
    if (prefix == 0xe4)  /* identity */
        return;
    /* orig[] shadows r[] so aliasing-style swizzle reads see the pre-modification value. */
    /* PPSSPP ApplyPrefixST defines out-of-width swizzles as the caller-supplied
     * invalid value (zero for ordinary ops).  Leaving these lanes uninitialized made
     * scalar compares depend on native stack contents and caused codegen/interpreter
     * divergence under legal prefix state. */
    float orig[4] = {0.0f,0.0f,0.0f,0.0f};
    for (int i = 0; i < n; i++)
        orig[i] = r[i];
    for (int i = 0; i < n; i++) {
        int regnum = (prefix >> (i * 2)) & 3;
        int absbit = (prefix >> (8 + i)) & 1;
        int constant = (prefix >> (12 + i)) & 1;
        int negate = (prefix >> (16 + i)) & 1;
        if (!constant) {
            r[i] = orig[regnum];
        } else {
            r[i] = SR_VFPU_CONST[regnum + (absbit << 2)];
        }
        /* Sign/absmask the float via a memcpy round-trip -- avoids the UB of casting
         * a float* to uint32_t* (strict-aliasing). The single-step xor with the sign
         * bit and AND with the abs mask is the same operation PPSSPP performs via
         * its FloatBits union. */
        {
            uint32_t bits;
            memcpy(&bits, &r[i], sizeof(bits));
            if (!constant && absbit)
                bits &= 0x7FFFFFFFu;
            if (negate)
                bits ^= 0x80000000u;
            memcpy(&r[i], &bits, sizeof(r[i]));
        }
    }
}

static float sr_clamp(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

void sr_vwrite(CpuState *s, const uint8_t *idx, float *d, int n, uint32_t dprefix) {
    if (dprefix) {
        for (int i = 0; i < n; i++) {
            int sat = (dprefix >> (i * 2)) & 3;
            if (sat == 1)
                d[i] = sr_clamp(d[i], 0.0f, 1.0f);
            else if (sat == 3)
                d[i] = sr_clamp(d[i], -1.0f, 1.0f);
        }
    }
    for (int i = 0; i < n; i++)
        if (!((dprefix >> (8 + i)) & 1))  /* write mask */
            s->v[idx[i]] = d[i];
}

/* ---- dispatch table (issue #45) ----
 *
 * The data structure and its lock-free primitives live in dispatch_table.h so the exact
 * production logic can be exercised host-neutrally (src/rt/dispatch_selftest.c). This file
 * owns the single live instance and the external sr_register/sr_lookup entry points that
 * the generated chunks and dispatch() call.
 *
 * Occupancy is carried in a dedicated `state` field there, independent of the key, so guest
 * code address 0 -- a real function on this zero-based image -- registers and resolves like
 * any other address. The previous `addr == 0` empty-slot sentinel (and the `addr != 0` L1
 * guard) made sr_lookup(0) impossible; see the header and the selftest. Making 0
 * representable does NOT make a *computed* dispatch target of 0 execute f_00000000: that is
 * a null-pointer call, handled by NULL_CALL_B in dispatch() before any lookup runs. */

static SrDispatchTable g_dtab;

/* Per-process counter of sr_register() calls. The generated main recomp.c compares
 * this against the expected function count emitted by codegen.py at compile time
 * to catch missing-chunk silent failures: if a chunk file's .o is missing from
 * the link (e.g. Makefile wildcard drift), its sr_register_chunk_N() never
 * executes and the count drops below the expected total. */
static uint32_t s_register_count = 0;

void sr_register(uint32_t addr, RecompFn fn) {
    sr_dtab_register(&g_dtab, addr, (uintptr_t)fn);
    s_register_count++;
}

uint32_t sr_register_count(void) { return s_register_count; }

RecompFn sr_lookup(uint32_t addr) {
    return (RecompFn)sr_dtab_lookup(&g_dtab, addr);
}

/* ---- dispatch hook table ---- */

typedef int (*HookResult)(CpuState *s, uint32_t target);
/* return 0 = consumed (early return), 1 = fall through to sr_lookup */

typedef struct {
    uint32_t key;
    uint32_t mask;   /* 0xFFFFFFFFu for single-address; range mask otherwise */
    const char *name;
    HookResult fn;
} DispatchHook;

/* --- exact-match hook handlers --- */

static int hook_log_alloc_req(CpuState *s, uint32_t target) {
    (void)target;
    static int trace = -1;
    if (trace < 0) trace = getenv("SR_ALLOC_TRACE") ? 1 : 0;
    if (trace) {
        fprintf(stderr, "ALLOC_REQ: size=%u from ra=0x%x\n", s->r[4], s->r[31]);
        fprintf(stderr, "  args a0=0x%x a1=0x%x a2=0x%x a3=0x%x sp=0x%08x\n",
                s->r[4], s->r[5], s->r[6], s->r[7], s->r[29]);
        uint32_t cpc = s->r[31] ? (s->r[31] - 4u) & ~1u : 0;
        if (cpc) fprintf(stderr, "  caller_pc=0x%08x insn=0x%08x\n", cpc, MEM_R32(cpc));
        for (int i = 0; i < 6; i++) fprintf(stderr, "  sp[%d]=0x%08x\n", i, MEM_R32(s->r[29] + (uint32_t)i*4u));
        fflush(stderr);
    }
    return 1;  /* fall through */
}

static int hook_log_free_req(CpuState *s, uint32_t target) {
    (void)target;
    if (getenv("SR_ALLOC_TRACE")) {
        fprintf(stderr, "FREE_REQ: ptr=0x%08x from ra=0x%x\n", s->r[4], s->r[31]);
        fflush(stderr);
    }
    return 1;  /* fall through */
}

static int hook_call_0x30948(CpuState *s, uint32_t target) {
    (void)target;
    /* Tail-call variants: real PSP code in caller `f_00030874` does `jr $t9` to a function
     * pointer loaded into `$t9` that targets `f_00030948+8` (= 0x30950). The +8 skips the
     * prologue `27bdffd0` (sp -= 0x30) on PSP where the SP was already set up. In our recomp,
     * f_00030948 IS registered but 0x30950 is not. Redirect by entering at the recomp's
     * function entry; the prologue's sp -= 0x30 is balanced by epilogue sp += 0x30, so net
     * stack delta is zero. Launcher's f_00030874 has already passed its epilogue so writing
     * into f_00030948's local region (offsets 0..0x24) does not corrupt live state. */
    RecompFn fb = sr_lookup(0x00030948u);
    if (fb) { fb(s); return 0; }
    return 1;
}

static int hook_hash_insert_guard(CpuState *s, uint32_t target) {
    /* f_0001b6c4: hash insert (linear probe). a0=hash_struct [+0]=arr [+4]=cap.
     * If cap==0 the probe wraps forever; guard: skip insert and return 0. */
    uint32_t htable = s->r[4];
    uint32_t cap = htable ? MEM_R32(htable + 4) : 0;
    static int hd = 0;
    if (hd < 8)
        fprintf(stderr, "HINSERT[%d]: htable=0x%08x cap=%u arr=0x%08x entry=0x%08x\n",
                hd++, htable, cap, htable ? MEM_R32(htable) : 0, s->r[5]);
    if (cap == 0) { s->r[2] = 0; s->pc = s->r[31]; return 0; }
    RecompFn fhi = sr_lookup(target);
    if (fhi) fhi(s);
    return 0;
}

static int hook_hash_fill_trace(CpuState *s, uint32_t target) {
    /* f_0001b584: hash_fill — outer loop calling f_0001b6c4 per entry. Trace entry. */
    static int hfd = 0;
    if (hfd < 4) {
        uint32_t ht = s->r[4], src = s->r[5];
        fprintf(stderr, "HFILL[%d]: htable=0x%08x cap=%u src=0x%08x count=%u\n",
                hfd++, ht, ht ? MEM_R32(ht+4) : 0, src, src ? MEM_R32(src+4) : 0);
    }
    RecompFn fhf = sr_lookup(target);
    if (fhf) fhf(s);
    return 0;
}

/* Recover the guest address of the call instruction that reached dispatch().
 *
 * s->pc is NOT it.  Generated code only assigns s->pc inside SR_YIELD, and SR_YIELD
 * only stores when the timeslice actually expires (recomp.h), so s->pc is the last
 * *preemption point*, which can be an arbitrary distance from the live instruction
 * and typically names some enclosing/earlier function.  Reading it as the faulting
 * site sends an investigation to the wrong function entirely.
 *
 * $ra is live and exact: MIPS jal/jalr set it to (call address + 8), and codegen
 * assigns it immediately before the dispatch, so the call site is $ra - 8.  Only
 * valid for the call forms that write $ra -- a `jr` tail-call carries the caller's
 * return address instead, so this is reported as approximate. */
static uint32_t sr_dispatch_call_site(const CpuState *s) {
    return s->r[31] >= 8u ? s->r[31] - 8u : 0u;
}

static int hook_null_call(CpuState *s, uint32_t target) {
    /* Current-HST runtime null-call policy (NOT a generic PSP rule). On this title's codegen
     * the two direct edges to offset 0 are emitted as direct f_00000000(s) calls and no
     * constant dispatch(s, 0) is emitted, so a computed target of 0 reaching dispatch is a
     * guest NULL pointer here, and this hook treats it as one: diagnose it (below) and return
     * to the caller (r2=0, pc=ra).
     *
     * This is compatibility/runtime policy, wired as NULL_CALL_B (key 0) and run BEFORE
     * sr_lookup in dispatch(). The dispatch TABLE is policy-free (dispatch_table.h):
     * sr_lookup(0) legitimately returns the offset-0 function, and this hook -- not the table
     * -- is what keeps a null indirect call from executing it. The GENERAL question (an
     * address-taken offset-0 pointer carried through guest data also arrives as integer 0,
     * and is not distinguishable from NULL without image/module identity) is unresolved under
     * #45 and its policy generalization is tracked by #20/#45. Do not move this decision into
     * the table, and do not remove this hook and let sr_lookup(0) resolve -- that would turn
     * a null call into a silent execution of the offset-0 function on this title. */
    static int null_call_n = 0;
    if (null_call_n < 5) {
        fprintf(stderr, "NULL_CALL[%d]: dispatch(target=0x%x) call_site~0x%08x ra=0x%08x "
                        "last_yield_pc=0x%08x uid=0x%x\n",
                null_call_n++, target, sr_dispatch_call_site(s), s->r[31],
                s->pc, sched_current_uid());
        fprintf(stderr, "  v0=0x%08x a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x\n",
                s->r[2], s->r[4], s->r[5], s->r[6], s->r[7]);
        fprintf(stderr, "  s0=0x%08x s1=0x%08x gp=0x%08x sp=0x%08x\n",
                s->r[16], s->r[17], s->r[28], s->r[29]);
        /* F3D disambiguator: distinguish "vtable slot genuinely zeroed in guest memory"
         * from "object/vptr pointer corrupted so the dispatch read the wrong slot".
         * a0 is the object; its +0 is the reloaded vptr; the constructors that fault here
         * (f_000649c0 / f_00064bf4) dispatch MEM[vptr+0xc]. Dump the object header, the
         * reloaded vptr, and the whole 0x3070c0 method-table window (image-valid:
         * +4=0x65888 +8=0x652e8 +0x14=0x64a08 +0x18=0x64a88). If those read 0 here, memory
         * was zeroed; if they're intact but a0/MEM[a0] is wrong, it's pointer corruption. */
        {
            uint32_t obj = s->r[4];
            uint32_t vptr = (obj && obj < 0x0c000000u) ? MEM_R32(obj) : 0xBADBAD00u;
            fprintf(stderr, "  F3D: obj=0x%08x MEM[obj+0]=0x%08x MEM[obj+4]=0x%08x  vptr_slot+0xc(disp_tgt)=0x%08x\n",
                    obj, vptr,
                    (obj && obj < 0x0c000000u) ? MEM_R32(obj + 4u) : 0xBADBAD00u,
                    (vptr < 0x0c000000u) ? MEM_R32(vptr + 0xcu) : 0xBADBAD00u);
            fprintf(stderr, "  F3D: table 0x3070c0: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                    MEM_R32(0x3070c0u), MEM_R32(0x3070c4u), MEM_R32(0x3070c8u), MEM_R32(0x3070ccu),
                    MEM_R32(0x3070d0u), MEM_R32(0x3070d4u), MEM_R32(0x3070d8u), MEM_R32(0x3070dcu));
        }
        fflush(stderr);
    }
    /* SR_NULLTRACE: dump the table singleton used by f_0006517c/f_00065104.
     * Its callers use `lui 0x35; lw -0x57b4`, which resolves to 0x0034a84c.
     * 0x34FFA84C is not a cached alias and does not occur in the guest code. */
    if (getenv("SR_NULLTRACE")) {
        static int nulltrace_n = 0;
        if (nulltrace_n < 5) {
            nulltrace_n++;
            fprintf(stderr, "SR_NULLTRACE[%d]: pc=0x%08x ra=0x%08x a0=0x%08x a1=0x%08x a2=0x%08x\n",
                    nulltrace_n, s->pc, s->r[31], s->r[4], s->r[5], s->r[6]);
            uint32_t table = MEM_R32(0x0034a84cu);
            fprintf(stderr, "  singleton[0x0034a84c]=0x%08x\n", table);
            if (table && table < 0x0c000000u) {
                for (uint32_t off = 0; off < 0x10u; off += 4u)
                    fprintf(stderr, "    [table+0x%02x] = 0x%08x\n", off, MEM_R32(table + off));
            }
            /* HLE pre-seed region for libfont (Task 2 Defensive seed) */
            fprintf(stderr, "  0x00333168 path (HLE seed region):\n");
            for (uint32_t off = 0; off < 0x28u; off += 4u) {
                fprintf(stderr, "    [+0x%02x] = 0x%08x\n", off, MEM_R32(0x00333168u + off));
            }
            /* The callers at ra=0x00293dd4 / 0x00293f10 pass that singleton as a0. */
            if (s->r[4]) {
                fprintf(stderr, "  a0-deref (caller's table ptr):\n");
                for (uint32_t off = 0; off < 0x20u; off += 4u) {
                    uint32_t v = MEM_R32(s->r[4] + off);
                    fprintf(stderr, "    [a0+0x%02x] = 0x%08x\n", off, v);
                }
            }
            fflush(stderr);
        }
    }
    s->r[2] = 0; s->pc = s->r[31]; return 0;
}

static int hook_resource_handle(CpuState *s, uint32_t target) {
    /* Catch and gracefully ignore execution of unallocated ECS/Resource handles.
     * Top 8 bits = Type ID, lower 24 bits = index; 0xFF... = unallocated sentinel. */
    if ((target & 0xff000000u) == 0x33000000u || (target & 0xff000000u) == 0x44000000u ||
        (target & 0xff000000u) == 0x55000000u || (target & 0xff000000u) == 0x88000000u ||
        target == 0x5b0ca3f8u || target == 0x27dfcb14u) {
        static int rh_warn = 0;
        if (rh_warn < 10) {
            fprintf(stderr, "RESOURCE_HANDLE: ignoring dispatch to raw handle 0x%08x from pc=0x%08x ra=0x%08x uid=0x%x\n",
                    target, s->pc, s->r[31], sched_current_uid());
            rh_warn++;
        }
        s->r[2] = 0;
        s->pc = s->r[31];
        return 0;
    }
    return 1;
}

static int hook_sceDmac_string(CpuState *s, uint32_t target) {
    (void)target;
    fprintf(stderr, "dispatch: intercepted known bad target 0x32305f34 (sceDmac string pointer), treating as nop\n");
    s->r[2] = 0;
    s->pc = s->r[31];
    return 0;
}

/* Hook for the module-registration table walk dispatch misses.
 * f_0000ef40 (module_table_walk) walks _reent + 0x148 (0x002cf480) and
 * dispatches through function pointers stored at offset 0x80 and 0x1c in
 * each table entry. Those pointers contain 0x002cf338 (the _reent struct
 * itself, a DATA address never used as a code target). The recompiled
 * walker calls dispatch(s, 0x002cf338) once per loop iteration per slot,
 * accumulating 926 miss messages and (worse) a same-target spin count
 * that triggers the spin guard.
 *
 * 0x002cf338 is the documented newlib data-as-fn-pointer sentinel: no
 * legitimate recompiled path dispatches there as a code address. We match
 * any dispatch to that target and return r2=0, consumed.
 *
 * Additionally, the hook short-circuits the inner module_table_walk
 * (f_0000ef40) itself when the dispatch table lookup reaches it from
 * outside. That replaces the original f_0000ef40 (which would otherwise
 * loop indefinitely) with a one-shot version that calls f_00011600 for
 * side effects and emits a success marker.
 *
 * NOTE: f_0000ef40 in this recomp is invoked via direct JAL inside the
 * compiled body, so it does NOT go through the dispatch table. The walk
 * the compiled f_0000ef40 emits is short-circuited by the 0x002cf338
 * target hook above; the f_0000ef40 hook below is a defensive fallback.
 */
static int hook_modtable_walk(CpuState *s, uint32_t target) {
    (void)target;
    /* Target 0x002cf338 is the newlib _reent struct, a data address. No
     * legitimate code path should dispatch to it. Return 0 to the caller. */
    static int walk_count = 0;
    if (walk_count < 4) {
        fprintf(stderr, "MODTABLE_WALK: skipped data-as-fn-pointer dispatch to 0x002cf338 "
                "from pc=0x%08x ra=0x%08x uid=0x%x\n",
                s->pc, s->r[31], sched_current_uid());
    }
    walk_count++;
    if ((walk_count & 3) == 0) {
        sr_yield(s);  /* yield periodically during modtable walk to keep vblanks alive */
    }
    s->r[2] = 1;
    s->pc = s->r[31];
    return 0;
}

static int hook_mod_stub(CpuState *s, uint32_t target) {
    (void)target;
    s->r[2] = 1;  /* return success for module function pointer */
    s->pc = s->r[31];
    return 0;  /* consumed */
}

/* The libfont fake-vtable dispatch path (0x0B0002xx trampolines + the 0x5fc4cb66 "garbage"
 * hook) was removed in F2: F1 proved it never fires (0 hits) — the sceFont library layer is now
 * HLE'd natively in src/rt/hle.c (real FontLibrary/Font structs from the game's allocator), so
 * there is no synthetic guest vtable to trampoline through. */

static int hook_fmt_trace(CpuState *s, uint32_t target) {
    /* HST: Format parser integer handler. Just log and let it run. */
    static unsigned long long fmt_count = 0;
    static uint32_t last_r19 = 0;
    fmt_count++;
    if (fmt_count <= 5 || fmt_count == 10 || (fmt_count & 0xFF) == 0) {
        uint32_t r19 = s->r[19];
        uint32_t flags = MEM_R32(s->r[29] + 0x228);
        fprintf(stderr, "FMT[%llu]: uid=0x%x r19=0x%08x flags=0x%08x ra=0x%08x\n",
                fmt_count, sched_current_uid(), r19, flags, s->r[31]);
        if (r19 != last_r19) {
            fprintf(stderr, "  str: \"");
            for (int i = 0; i < 64 && MEM_R8(r19 + i) != 0; i++)
                fputc(MEM_R8(r19 + i), stderr);
            fprintf(stderr, "\"\n");
        }
        last_r19 = r19;
        fflush(stderr);
    }
    /* Let it run: call the recompiled handler */
    RecompFn fn = sr_lookup(target);
    if (fn) fn(s);
    return 0;
}

static int hook_thunk_call_trace(CpuState *s, uint32_t target) {
    /* Log thunk dispatches (function pointer calls through 0xec0/0xee4) */
    uint32_t fptr = MEM_R32(0x2CED08u);
    fprintf(stderr, "THUNK_CALL: tgt=0x%x fptr_at_2CED08=0x%x uid=0x%x ra=0x%x\n",
            target, fptr, sched_current_uid(), s->r[31]);
    fflush(stderr);
    return 1;  /* fall through — does NOT return early */
}

/* --- exact-match dispatch table (single-address hooks) --- */

/* f_000008d8 — table/resource walker (31 caller sites: init_array walk, libfont
 * init, resource array walk, security_array_walk, modtable walk...).
 *
 * The walker loop at L_00000940 dispatches to sub-routines via f_000008d8's
 * own dispatch slot (lines 9525/9566/9646). Because f_000008d8 is registered
 * in sr_register, dispatch goals land back in the walker body at L_00000940,
 * which loops while `r[4] < r[18]`. The per-iteration cap (0x0000095c /
 * WALKER_CAP) fires after at most 2048 iterations and forces exit by setting
 * `r[2]=0`, but each iteration yields once; the scheduler keeps returning to
 * the same worker just to reach the next yield site, creating an enormous
 * busy-spin at `pc=0x000008d8` that starves frame-present progress.
 *
 * In every known caller context the result is used as a count: the caller
 * tests `r[2] <= 0` to decide whether to enter a follow-up loop. Returning 0
 * is therefore semantically equivalent to "walker completed, nothing to do"
 * and is exactly what the WALKER_CAP exit state would have signalled after
 * the 2048 iterations. Bypassing the walker body cuts the 2048-iteration,
 * 2048-yield spin to a single tick.
 *
 * Fix: set r2=0 and return to the caller directly. Logged once via
 * WALKER_SKIP so the bypass is visible in SR_DEBUG output.
 */

/* f_0000d62c — config-load tokenizer wrapper (calls f_00016178 → f_00015fb4
 * to scan a CSV/offsets buffer for a terminator byte). The config loop at
 * L_00047f48 (f_00047d7c / Config_LoadGameSettings) polls this function:
 *   r2 = f_0000d62c();
 *   r2 += 0x7ff;
 *   if (r2 < 0) loop;   // exit when signed-non-negative
 *
 * The PSP tokenizer scans a real loaded file; in HLE it never finds the
 * expected terminator state and keeps returning negative, so the loop spins
 * forever. The worker is exclusively stuck at this pc=0x0000d62c busy-wait
 * (confirmed by SR_THLOG). Fake "end of tokens" (r2=0) so the loop exits,
 * letting the worker reach the frame-present path. */

 /* f_00049200 — "device operation complete?" poll used by the display/GE
 * completion checks (f_000491cc, f_00049194, ...). Reads device
 * state (MEM[0x311140] / MEM[0x2d0738]) and returns 1 when complete. In HLE
 * the PSP ME never drives these devices, so it can never become complete and
 * any poll loop spins forever (the frame-1+ stall at pc=0x000491cc). Fake
 * completion so the frame loop can advance. */

/* f_000487f4 — "display/vblank device ready" predicate used by the per-frame
 * render-wait loop (L_00046dec in the worker's main loop: issues render, polls
 * f_000487f4, spins via SR_YIELD until it returns true, then flips).
 *
 * It reads MEM[0x34B328] and several other PSP-ME kernel device pointers that
 * are normally seeded by the PSP Media Engine at boot — our HLE never seeds
 * them, so the predicate can never become true and the loop spins forever
 * (exactly the "one frame presented, then watchdog abort" symptom).
 *
 * Fix: call the real function; if it reports NOT ready (0), fake readiness so
 * the frame loop can proceed. Real behavior is preserved when the device
 * state ever is seeded. */

/* f_0004f6b4 — render command-format lookup (string-match + 0x240-entry
 * halfword format-scan dispatcher). It houses the local label L_0004f7dc and
 * the inner scan loop at L_0004f834. The original f_0004f7dc interception was DEAD
 * code: f_0004f7dc only exists as a local label inside this function, so the
 * linker never intercepted it. This wrap targets the real exported function.
 *
 * Background: on real PSP a display-driver background thread seeds the format
 * table and an interrupt wakes the caller; our HLE has no such thread, so the
 * scan can't find a valid entry. (Codegen's self-loop guard already caps the
 * 0x4f834 beq $0,$0,-1 at 4 iterations, so it is no longer an infinite spin —
 * but bypassing the whole scan here is faster and removes any ambiguity.)
 *
 * Fix: return slot index 0 in r2 (the renderer only needs a valid non-(-1)
 * slot) and return via the caller's RA so the render path proceeds. Logged
 * once via RENDERFMT_SKIP so the skip is visible in SR_DEBUG output. */

/* 0x0000100c — PLT/jump-table trampoline. The PSP dynamic linker fills a
 * per-dispatch table with function pointers; the recompiler materialises each
 * PLT entry as a small stub that begins with a jump to 0x0100c. In the SR
 * runtime, sr_lookup(0x0100c) hits sr_register's table and lands here.
 *
 * When no HLE equivalent is registered for a particular slot, the slot value
 * stays 0x0100c (self-referential) and the worker enters an infinite
 * `dispatch -> f_0000100c -> dispatch -> f_0000100c` spin. This is the
 * `pc=0x0000100c` stall observed after the walker bypass.
 *
 * Fix: log the miss and return 0 to the PLT caller. The callers that use
 * these PLT slots generally test the return value or proceed conditionally,
 * so returning 0 is harmless and unblocks the init sequence. */
static int hook_plt_unimpl(CpuState *s, uint32_t target) {
    (void)target;
    static int s_warned = 0;
    if (!s_warned) {
        fprintf(stderr, "PLT_SKIP: trampoline 0x0000100c invoked"
                " (target=0x%08x r25=0x%08x uid=%x)\n",
                s->r[25] & 0x3FFFFFFFu, s->r[25], sched_current_uid());
        fflush(stderr);
        s_warned = 1;
    }
    s->r[2] = 0u;
    return 0; /* consumed */
}

static int hook_plt_walk(CpuState *s, uint32_t target) {
    (void)s;
    (void)target;
    return 1; /* fall through — walker loop continues to next entry */
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) void f_00304290(CpuState *s) { (void)s; }
#endif

static int hook_init_lang(CpuState *s, uint32_t target) {
    (void)target;
    extern void f_00304290(CpuState *s);
    f_00304290(s);
    MEM_W8(0x0030fbfdu, 1u);  /* force JP flag: CSV loader requires it for asset path selection */
    return 0; /* handled completely; callers must not call f_00304290 again */
}

static const DispatchHook g_exact_hooks[] = {
    { 0x00304290u, 0xFFFFFFFFu, "INIT_LANG",        hook_init_lang },
    { 0x000104b0u, 0xFFFFFFFFu, "ALLOC_REQ",        hook_log_alloc_req },
    { 0x000104e0u, 0xFFFFFFFFu, "FREE_REQ",         hook_log_free_req },
    { 0x00030950u, 0xFFFFFFFFu, "TC30950",          hook_call_0x30948 },
    { 0x0001b6c4u, 0xFFFFFFFFu, "HINSERT",          hook_hash_insert_guard },
    { 0x0001b584u, 0xFFFFFFFFu, "HFILL",            hook_hash_fill_trace },
    { 0x656a6f72u, 0xFFFFFFFFu, "NULL_CALL_A",      hook_null_call },
    { 0x00000000u, 0xFFFFFFFFu, "NULL_CALL_B",      hook_null_call },
    { 0x32305f34u, 0xFFFFFFFFu, "SCEDMAC",          hook_sceDmac_string },
    { 0x00018130u, 0xFFFFFFFFu, "FMT_TRACE",        hook_fmt_trace },
    { 0x0000ef40u, 0xFFFFFFFFu, "MODTABLE_WALK",    hook_modtable_walk },
    { 0x002cf338u, 0xFFFFFFFFu, "_REENT_DATA",      hook_modtable_walk },
    { 0x0B000100u, 0xFFFFFFFFu, "MOD_STUB",          hook_mod_stub },
{ 0x00000ec0u, 0xFFFFFFFFu, "THUNK_A", hook_thunk_call_trace },
{ 0x00000ee4u, 0xFFFFFFFFu, "THUNK_B", hook_thunk_call_trace },
{ 0x0000100cu, 0xFFFFFFFFu, "PLT_TRAMP", hook_plt_unimpl },
{ 0x00102e1cu, 0xFFFFFFFFu, "PLT_WALK_1", hook_plt_walk },
{ 0x001030b0u, 0xFFFFFFFFu, "PLT_WALK_2", hook_plt_walk },
{ 0, 0, NULL, NULL } /* sentinel */
};

/* --- range-match dispatch table (address-range hooks) --- */
/* Range hooks manage their own address predicates internally; key/mask are
 * documentation-only. The dispatch loop calls every entry unconditionally. */
static const DispatchHook g_range_hooks[] = {
    { 0, 0, "RESOURCE_HANDLE",   hook_resource_handle },
    { 0, 0, NULL, NULL }  /* sentinel */
};

#define MISS_HASH_SIZE 128
typedef struct { uint32_t target; uint32_t pc; uint32_t ra; } DispatchMissEntry;
static DispatchMissEntry g_miss_table[MISS_HASH_SIZE];
static int g_miss_count = 0;

static void dump_dispatch_misses(void) {
    if (g_miss_count == 0) return;
    fprintf(stderr, "\n--- UNIQUE DISPATCH MISSES SUMMARY (%d entries) ---\n", g_miss_count);
    for (int i = 0; i < MISS_HASH_SIZE; i++) {
        if (g_miss_table[i].target != 0) {
            fprintf(stderr, "  target=0x%08x pc=0x%08x ra=0x%08x\n",
                    g_miss_table[i].target, g_miss_table[i].pc, g_miss_table[i].ra);
        }
    }
    fprintf(stderr, "--- END UNIQUE DISPATCH MISSES SUMMARY ---\n\n");
    fflush(stderr);
}

void dispatch(CpuState *s, uint32_t target) {
    /* A null callback terminates f_0003dfd0's circular callback-list traversal. The
     * permissive miss path normally returns 0 ("continue"), so handle this call site
     * before the generic null-PC guard. */
    if (target == 0u && s->r[31] == 0x0003e06cu) {
        s->r[2] = 1u;
        s->pc = s->r[31];
        return;
    }
    /* Per-instruction VFPU fallback.  Codegen keeps executing the owning native C
     * function after this returns, so no guest function boundary or host stack frame is
     * introduced.  Keeping the interpreter entry here also gives all computed execution
     * paths one authoritative fallback instead of duplicating decode logic in generated C. */
    if ((target & SR_DISPATCH_VFPU_MASK) == SR_DISPATCH_VFPU_TAG) {
        uint32_t pc=target&~SR_DISPATCH_VFPU_MASK;
        uint32_t op=MEM_R32(pc);
        if (sr_vfpu_interp(s,op)==SR_VFPU_OTHER) {
            fprintf(stderr,
                    "VFPU_FALLBACK_OTHER: pc=0x%08x op=0x%08x target=0x%08x caller_pc=0x%08x ra=0x%08x sp=0x%08x "
                    "a0=0x%08x a1=0x%08x a2=0x%08x obj_vptr=0x%08x vslot0c=0x%08x "
                    "font_tbl=0x%08x font_vec=0x%08x font_slot0=0x%08x\n",
                    pc, op, target, s->pc, s->r[31], s->r[29], s->r[4], s->r[5], s->r[6],
                    s->r[4] < 0x0c000000u ? MEM_R32(s->r[4]) : 0xdeadu,
                    (s->r[4] < 0x0c000000u && MEM_R32(s->r[4]) < 0x0c000000u)
                        ? MEM_R32(MEM_R32(s->r[4]) + 0x0cu)
                        : 0xdeadu,
                    MEM_R32(0x034a84cu),
                    MEM_R32(0x034a84cu) < 0x0c000000u ? MEM_R32(MEM_R32(0x034a84cu) + 4u) : 0xdeadu,
                    (MEM_R32(0x034a84cu) < 0x0c000000u && MEM_R32(MEM_R32(0x034a84cu) + 4u) < 0x0c000000u)
                        ? MEM_R32(MEM_R32(MEM_R32(0x034a84cu) + 4u))
                        : 0xdeadu);
            sr_unimplemented(pc,"VFPU runtime interpreter returned SR_VFPU_OTHER");
        }
        s->pc=pc+4;
        return;
    }
    /* f_0003dfd0 walks a circular callback list. Its callback reaches the indirect call
     * at 0x292fa0 with -1 as a terminal target; treating that as a permissive miss
     * returns 0, which means "continue" to the outer walker and loops at 0x3e06c.
     * Report completion only for this exact inner call site. */
    if (target == UINT32_MAX && s->pc == 0x00292fa0u && s->r[31] == 0x00047a0cu) {
        s->r[2] = 1u;
        s->pc = s->r[31];
        return;
    }
    /* CpuState corruption guard: a PC of 0 means the thread state has collapsed.
     * Terminate the thread to prevent infinite spinning/deadlock. */
    if (s->pc == 0) {
        s->status = 0;
        if (sched_current_uid() != 0) {
            sched_terminate_thread(sched_current_uid());
        }
        return;
    }
    static int plt_miss_streak = 0;  /* PLT consecutive-miss counter for force-terminate */
    /* The entry guard above (`if (s->pc == 0)`, near the top of this function) already
     * catches s->pc==0 unconditionally and returns before anything else runs, so a second
     * "saved PC of 0" check here — gated on the same s->pc with no intervening write to it —
     * could never fire. Removed as dead code (historical cleanup: collapse the pc==0
     * guards into a single check"). The remaining post-SR_YIELD guard below is NOT
     * redundant with the entry guard: SR_YIELD can switch fiber context underneath `s`,
     * so it observes a genuinely different point in execution and stays as its own check.
     *
     * The every-100-dispatch sr_timeslice=0 resets that used to sit here (one for the
     * worker thread, one for every other thread) were a defensive backstop against
     * priority inversion predating LAUNCHER_DEMOTE (src/rt/sched.c, ~line 597): the
     * launcher thread is unconditionally demoted below the worker's priority at thread
     * creation now, which is the real fix. Removed per Phase 1.4 — pure dispatch overhead
     * once the primary fix is in place. */
    SR_YIELD(s, s->pc);

    /* Phase 2.6 / BUG1: pc=0 in dispatch() means the thread lost its state somewhere
     * (corrupt return, missed epilogue, scheduler scratch). Without this guard, we
     * dispatch into junk and the thread spins at the wrong PC forever, deadlocking
     * the scheduler. Treat the thread as stopped and yield once so the scheduler
     * can keep running siblings. */
    if (s->pc == 0u) {
        if (sched_current_uid() != 0) {
            static int pc0_n = 0;
            if (pc0_n < 4) {
                fprintf(stderr, "dispatch: pc==0 uid=0x%x ra=0x%08x target=0x%08x — halting thread\n",
                        sched_current_uid(), s->r[31], target);
                pc0_n++;
            }
            sched_exit_current(0);
            return;
        }
    }

    /* Record every dispatch in circular trace buffer.
     * The trace is circular by design (DISPATCH_TRACE_SIZE = 32); g_dtrace_idx wraps
     * via `idx % DISPATCH_TRACE_SIZE`. To prevent the bare integer from overflowing on
     * a pathologically long run and producing integer-undefined behaviour, mask back
     * into range once it crosses INT_MAX. dump_dispatch_trace uses the same formula. */
    {
        int di = g_dtrace_idx % DISPATCH_TRACE_SIZE;
        g_dtrace[di].target = target;
        g_dtrace[di].pc = s->pc;
        g_dtrace[di].ra = s->r[31];
        g_dtrace[di].uid = sched_current_uid();
        g_dtrace[di].used = 1u;
        g_dtrace_idx++;
        if (g_dtrace_idx >= (1 << 30)) g_dtrace_idx = (g_dtrace_idx & (DISPATCH_TRACE_SIZE - 1));
    }

    /* getenv() in a hot path walks the host env block every dispatch; cache it on the
     * first call. The "-1 / 0 / 1" tristate is a common idiom that races if two
     * fibers enter concurrently, but sr_hle_init runs single-threaded during boot
     * so the initial probe is safe; subsequent dispatchers across fibers see the
     * resolved value via atomic-ish visibility on Windows/x86 (TSO). */
    static int s_displog = -1;
    if (s_displog < 0) s_displog = getenv("SR_DISPLOG") ? 1 : 0;
    if (s_displog)
        fprintf(stderr, "DISPATCH 0x%08x from 0x%08x (ra=0x%08x)\n", target, s->pc, s->r[31]);

    /* Table-driven dispatch hooks (exact-match, preserves source order) */
    for (const DispatchHook *h = g_exact_hooks; h->fn; h++) {
        if (h->mask == 0xFFFFFFFFu ? target == h->key
                                   : (target & h->mask) == (h->key & h->mask)) {
            extern int g_hle_depth;
            g_hle_depth++;
            int rc = h->fn(s, target);
            g_hle_depth--;
            if (rc == 0) return;  /* consumed */
            /* rc == 1: fall through (trace-only hook) */
        }
    }

    /* Table-driven dispatch hooks (range/predicate, self-filtering) */
    for (const DispatchHook *h = g_range_hooks; h->fn; h++) {
        extern int g_hle_depth;
        g_hle_depth++;
        int rc = h->fn(s, target);
        g_hle_depth--;
        if (rc == 0) return;  /* consumed */
    }

    /* Diagnostic: the launcher's init-array walker (f_00000fa0, dispatch site 0x00000fdc)
     * iterates an array of function pointers at r4[0..r5) calling each via dispatch. When
     * those targets miss, the array is unpopulated/corrupt. Log the walk parameters to
     * identify the gap. */
    if (s->pc == 0x00000fdcu) {
        static int initarr_n = 0;
        if (initarr_n < 50) {
            initarr_n++;
            fprintf(stderr, "INIT_ARRAY_WALK: base=0x%08x count=0x%08x idx=0x%08x target=0x%08x\n",
                    s->r[4], s->r[5], s->r[16], target);
            fflush(stderr);
        }
    }

    /* Diagnostic: identify who invokes the launcher's init-array walker (f_00000fa0) with a
     * (possibly bogus) base/count. Match any MIPS segment form (kuseg/kseg0/kseg1/reloc). */
    if ((target & 0x0FFFFFFFu) == 0x00000fa0u) {
        static int fa0_n = 0;
        if (fa0_n < 30) {
            fa0_n++;
            fprintf(stderr, "CALL_F_00000FA0: caller_pc=0x%08x ra=0x%08x r4=0x%08x r5=0x%08x r6=0x%08x r7=0x%08x\n",
                    s->pc, s->r[31], s->r[4], s->r[5], s->r[6], s->r[7]);
            fflush(stderr);
        }
    }

    RecompFn fn = sr_lookup(target);
    /* PSP relocation base fallback: game vtable/data contain absolute code addresses
     * offset by 0x08000000 (PSP user RAM base), but the recompiler loads the ELF at
     * 0x00000000.  When sr_lookup(target) fails for an address in [0x08000000..0x0C000000),
     * try the same address minus the PSP RAM base to find the recompiled function. */
    if (!fn && target >= 0x08000000u && target < 0x0C000000u) {
        fn = sr_lookup(target - 0x08000000u);
        if (fn) {
            static int reloc_fixup_n = 0;
            if (reloc_fixup_n < 20) {
                fprintf(stderr, "RELOC_FIXUP: target 0x%08x -> 0x%08x (found recompiled fn)\n",
                        target, target - 0x08000000u);
                reloc_fixup_n++;
            }
            target = target - 0x08000000u;  /* use corrected target for logging */
        }
    }
    /* MIPS segment-bit normalization. The game returns through `jr $ra` with cached/
     * uncached kernel-segment addresses (kseg0 0x80000000..0x9FFFFFFF, kseg1
     * 0xA0000000..0xBFFFFFFF) and 0xC0000000+ kernel-window targets. The recompiler
     * maps all code to a flat image-relative (0-based) space, so strip the segment bits
     * (SR_PHYS mask) before the lookup. Without this, those returns miss the table and the
     * thread spins forever on "dispatch miss" (observed: the launcher thread never reaches
     * sceDisplaySetFrameBuf, so no frame is ever presented -> black screen / watchdog). */
    if (!fn) {
        uint32_t phys = target & 0x1FFFFFFFu;
        if (phys != target) {
            fn = sr_lookup(phys);
            if (fn) {
                static int kseg_fixup_n = 0;
                if (kseg_fixup_n < 20) {
                    fprintf(stderr, "KSEG_FIXUP: target 0x%08x -> 0x%08x (found recompiled fn)\n",
                            target, phys);
                    kseg_fixup_n++;
                }
                target = phys;  /* use normalized target for logging */
            }
        }
    }
    /* Late-import bridge (Track B contract, sr_hle_resolve_late_import in hle.c).
     * A miss that survives the reloc/kseg fixups above is usually a call into a
     * runtime-loaded module (dynamic import stub, e.g. launcher pc 0x0000efec) whose
     * export table was populated after static codegen. Ask the late-import registry:
     *   - a rebased guest export address: patch the dispatch table so every future
     *     lookup of this target hits directly, then continue on the normal hit path;
     *   - SR_HLE_LATE_BUILTIN: the id is a built-in HLE handler — trap into sr_syscall
     *     with the standard HLE return convention (v0 = result, pc = ra);
     *   - 0: unresolved — fall through to the existing miss handling unchanged. */
    if (!fn) {
        uint32_t late = sr_hle_resolve_late_import(target);
        if (late == SR_HLE_LATE_BUILTIN) {
            static int late_hle_n = 0;
            if (late_hle_n < 20) {
                fprintf(stderr, "LATE_IMPORT_HLE: target=0x%08x from pc=0x%08x -> sr_syscall\n",
                        target, s->pc);
                late_hle_n++;
            }
            sr_hle_call(s, target);
            return;
        }
        if (late != 0 && late != target) {
            /* Normalize the resolved export the same way direct targets are. */
            uint32_t resolved = late;
            fn = sr_lookup(resolved);
            if (!fn && (resolved & 0x1FFFFFFFu) != resolved) {
                resolved &= 0x1FFFFFFFu;
                fn = sr_lookup(resolved);
            }
            if (fn) {
                /* Patch the runtime table: alias the missed target to the resolved
                 * body so subsequent dispatches skip the registry walk entirely. */
                sr_register(target, fn);
                static int late_patch_n = 0;
                if (late_patch_n < 20) {
                    fprintf(stderr, "LATE_IMPORT_PATCH: target=0x%08x -> 0x%08x (table patched)\n",
                            target, resolved);
                    late_patch_n++;
                }
                target = resolved;  /* use resolved target for logging below */
            } else {
                static int late_dangle_n = 0;
                if (late_dangle_n < 20) {
                    fprintf(stderr, "LATE_IMPORT_DANGLE: target=0x%08x resolved to 0x%08x "
                            "but no recompiled body — falling through to miss path\n",
                            target, late);
                    late_dangle_n++;
                }
            }
        }
    }
    /* Diagnostic note: the launcher's init-array walker (f_00000fa0) is INLINED into its
     * callers, so it is never entered via dispatch() — do not add a dispatch-entry probe for
     * it. Its loop body dispatches from pc 0x00000fdc; see INIT_ARRAY_WALK / INIT_ARRAY_DUMP
     * below for the instrumentation that actually fires. */
    if (fn) {
        if (target == 0x00000214u) {
            fprintf(stderr, "DISPATCH_EXIT: target=0x214 fn=%p ra=0x%x uid=0x%x\n",
                    (void*)fn, s->r[31], sched_current_uid());
            fflush(stderr);
        }
        /* The init-array walker (f_00000f98/f_00000fa0) uses s->r[16] (callee-saved s0
         * in MIPS convention) as its loop pointer over the function-pointer table. The
         * generated code saves r[16] at function entry but reads it back from the
         * CPUState struct — not from a local — so the dispatch targets below can and
         * do clobber it by writing to s->r[16] (a.k.a. s0). Once corrupted, the walker
         * reads MIPS instruction words from code addresses instead of function pointers,
         * triggering dispatch misses for the rest of the ~55 MB range it tries to walk.
         *
         * Save/restore r[16] around the call whenever the dispatch originates from
         * inside the walker's loop (pc == 0x00000fdc) or from the first-entry path
         * (pc == 0x00000f98, set by SR_YIELD at function entry). Without this the
         * very first init function called by the walker corrupts r[16] and the loop
         * spins to watchdog. */
        uint64_t start_ns = 0;
        SrProfEntry *prof_entry = NULL;
        if (g_prof_enabled) {
            prof_entry = prof_lookup(target);
            if (prof_entry) {
                prof_entry->call_count++;
            }
            start_ns = SDL_GetTicksNS();
        }

        if (s->pc == 0x00000f98u || s->pc == 0x00000fdcu) {
            uint32_t saved_r16 = s->r[16];
            if (s_displog) {
                fprintf(stderr, "  [INIT_WALKER_GUARD] saving r[16]=0x%08x before dispatch 0x%08x\n",
                        saved_r16, target);
            }
            fn(s);
            s->r[16] = saved_r16;
            if (s_displog) {
                fprintf(stderr, "  [INIT_WALKER_GUARD] restored r[16]=0x%08x (was 0x%08x after call)\n",
                        saved_r16, s->r[16]);
            }
        } else {
            if (s_displog) {
                fprintf(stderr, "  -> calling fn %p for 0x%08x, s->r[29]=0x%08x sr_timeslice=%d\n", (void*)fn, target, s->r[29], atomic_load_explicit(&sr_timeslice, memory_order_relaxed));
            }
            fn(s);
        }

        if (g_prof_enabled && prof_entry) {
            prof_entry->duration_ns += (SDL_GetTicksNS() - start_ns);
        }

        if (s_displog) {
            fprintf(stderr, "  <- returned from fn %p for 0x%08x, s->r[29]=0x%08x sr_timeslice=%d\n", (void*)fn, target, s->r[29], atomic_load_explicit(&sr_timeslice, memory_order_relaxed));
        }
        /* Log walker dispatch return values — these virtual method calls should
         * return string pointers; small values indicate corruption */
        if (s->pc >= 0x000650e0u && s->pc <= 0x000651b0u && s->r[2] < 0x10000u) {
            static int wlog_n = 0;
            if (wlog_n < 12) {
                fprintf(stderr, "WALKER_RET: caller=0x%08x target=0x%08x v0=0x%08x r4=0x%08x uid=0x%x\n",
                        s->pc, target, s->r[2], s->r[4], sched_current_uid());
                wlog_n++;
            }
        }
    } else {
        {
            static int miss_dump_registered = 0;
            if (!miss_dump_registered) {
                miss_dump_registered = 1;
                atexit(dump_dispatch_misses);
            }
            uint32_t h = (target * 2654435761u) & (MISS_HASH_SIZE - 1);
            int found = 0;
            for (int i = 0; i < 16; i++) {
                uint32_t idx = (h + i) & (MISS_HASH_SIZE - 1);
                if (g_miss_table[idx].target == target) { found = 1; break; }
                if (g_miss_table[idx].target == 0 && g_miss_count < MISS_HASH_SIZE) {
                    g_miss_table[idx].target = target;
                    g_miss_table[idx].pc = s->pc;
                    g_miss_table[idx].ra = s->r[31];
                    g_miss_count++;
                    fprintf(stderr, "DISPATCH_MISS_NEW[%d]: target=0x%08x caller_pc=0x%08x ra=0x%08x uid=0x%x\n",
                            g_miss_count, target, s->pc, s->r[31], sched_current_uid());
                    found = 1;
                    break;
                }
            }
            (void)found;
        }
        if (s->pc == 0x00000fdcu) {
            static int wt_dump = 0;
            if (wt_dump < 3) { wt_dump++; fprintf(stderr, "WALKER_BAD_MISS target=0x%08x\n", target); dump_dispatch_trace(); }
        }
        /* Suppress per-miss detail logging unless SR_DISPLOG is set — the unique miss
         * tracker above captures the important information without flooding stderr. */
        static int s_displog_miss = -1;
        if (s_displog_miss < 0) s_displog_miss = getenv("SR_DISPLOG") ? 1 : 0;
        if (s_displog_miss) {
            fprintf(stderr, "dispatch miss at 0x%08x from 0x%08x (ra=0x%08x) uid=0x%x\n",
                    target, s->pc, s->r[31], sched_current_uid());
            fprintf(stderr, "  r[2]=0x%08x r[4]=0x%08x r[5]=0x%08x r[6]=0x%08x r[7]=0x%08x\n",
                    s->r[2], s->r[4], s->r[5], s->r[6], s->r[7]);
            fprintf(stderr, "  r[16]=0x%08x r[17]=0x%08x r[18]=0x%08x r[19]=0x%08x\n",
                    s->r[16], s->r[17], s->r[18], s->r[19]);
        }
        /* f_002919d4 is a generic worker launcher: a1 points at an object pointer and
         * object+8 contains the three-word descriptor consumed by the 0x100c PSP PLT
         * resolver.  On a resolver miss the generated function has already overwritten
         * t9 with the final target, so reconstruct the inputs here while guest memory is
         * still live.  This is opt-in and does not alter resolution or thread state. */
        static int s_pltlog = -1;
        if (s_pltlog < 0) s_pltlog = getenv("SR_PLTLOG") ? 1 : 0;
        if (s->pc == 0x0000100cu && s_pltlog) {
            static int plt_detail_n = 0;
            if (plt_detail_n++ < 16) {
                uint32_t argp = s->r[5];
                uint32_t object = MEM_R32(argp);
                uint32_t key = MEM_R32(object + 4u);
                uint32_t d0 = MEM_R32(object + 8u);
                uint32_t d1 = MEM_R32(object + 12u);
                uint32_t d2 = MEM_R32(object + 16u);
                uint32_t adjusted = key + d0;
                uint32_t table = (int32_t)d1 < 0 ? 0u : MEM_R32(adjusted + d2);
                uint32_t resolved = (int32_t)d1 < 0 ? d2 : MEM_R32(table + d1);
                fprintf(stderr,
                        "  PLT_DETAIL: argp=0x%08x object=0x%08x key=0x%08x "
                        "descriptor=[0x%08x,0x%08x,0x%08x] adjusted=0x%08x "
                        "table=0x%08x resolved=0x%08x\n",
                        argp, object, key, d0, d1, d2, adjusted, table, resolved);
            }
        }
        /* Dump the fn-pointer array the launcher's init walker (0x00000fdc) is reading,
         * to tell an unpopulated/unrelocated table from a codegen pointer bug. */
        if (s->pc == 0x00000fdcu) {
            static int dump_n = 0;
            if (dump_n < 2) {
                dump_n++;
                uint32_t a0 = s->r[16] & ~0xfu;
                fprintf(stderr, "  INIT_ARRAY_DUMP @0x%08x:\n", a0);
                for (uint32_t o = 0; o < 16u * 4u; o += 4u) {
                    uint32_t w = MEM_R32(a0 + o);
                    fprintf(stderr, "    0x%08x: 0x%08x%s\n", a0 + o, w,
                            (w == target) ? "  <== current" : "");
                }
            }
        }
        /* Phase 2.3: PLT trampolines live in [0x00001000..0x000010FF]. They resolve import
         * targets via GOT tables, but PSP kernel import resolution never ran, so GOT
         * entries produce garbage targets. Legitimate PLT dispatches succeed via sr_lookup
         * above; only garbage targets reach here. Return 0 (failure) so the caller sees
         * an honest linkage failure instead of a phantom success at address 0x1.
         * Narrowed range: original check was s->pc < 0x10000 which was too broad. */
        if (s->pc >= 0x00001000u && s->pc <= 0x000010FFu) {
            static int plt_miss_n = 0;
            plt_miss_streak++;
            if (plt_miss_n < 8) {
                fprintf(stderr, "  PLT_MISS: caller=0x%08x target=0x%08x ra=0x%08x — returning 0\n",
                        s->pc, target, s->r[31]);
                plt_miss_n++;
            }
            /* After 5000 consecutive PLT misses, the caller is almost certainly
             * an infinite table-walk loop whose GOT was never resolved. Force
             * terminate by setting the iterator (sp+0x24) so the loop's exit
             * check (iterator+4 == sentinel at sp+0x20) fires immediately. */
            if (plt_miss_streak == 5000) {
                uint32_t sp = s->r[29];
                uint32_t sentinel = MEM_R32(sp + 0x20u);
                MEM_W32(sp + 0x24u, sentinel - 4u);
                fprintf(stderr, "  PLT_MISS: force-terminated loop at streak 5000 "
                        "(sentinel=0x%08x, sp=0x%08x)\n", sentinel, sp);
            }
            s->r[2] = 0u;  /* Honest failure: import not resolved */
            s->pc = s->r[31];
            return;
        }
        plt_miss_streak = 0;  /* Reset on any non-PLT dispatch */
        /* Boot-progress fix: a dispatch miss into a statically-unrecompiled region
         * (e.g. a function pointer stored in a vtable pointing into a runtime-loaded
         * PRX module, or a codegen-missed function) is no longer fatal. Return 0 (safe
         * sentinel) to the caller so boot proceeds and we can observe subsequent misses.
         * Set SR_DISPATCH_FATAL=1 to restore the old exit(1) when chasing the FIRST miss.
         * s->pc + 8 handles both jalr (where ra = s->pc + 8, same as old s->r[31]) and
         * jr tail-call thunks (advances past the jr+delay-slot, breaking infinite loops). */
        static int nonplt_miss_n = 0;
        if (nonplt_miss_n < 64) {
            fprintf(stderr, "  NONPLT_MISS: returning 0 (sentinel) from pc=0x%08x new_pc=0x%08x ra=0x%08x\n",
                    s->pc, s->pc + 8, s->r[31]);
            nonplt_miss_n++;
        }
        if (getenv("SR_DISPATCH_FATAL")) {
            exit(1);
        }
        s->r[2] = 0;
        s->pc = s->pc + 8;
        return;
    }
}

jmp_buf g_hle_jmp;
int g_hle_depth = 0;

void sr_hle_call(CpuState *s, uint32_t nid) {
    sr_hit_hle = 1;
    extern uint32_t sr_syscall(CpuState *s, uint32_t nid);
    g_hle_depth++;
    s->r[2] = sr_syscall(s, nid);
    g_hle_depth--;
    s->pc = s->r[31];
}

void sr_unimplemented(uint32_t pc, const char *reason) {
    fprintf(stderr, "sr_unimplemented: function 0x%08x: %s\n", pc, reason);
    abort();
}

/* ---- tracing ---- */

static FILE *s_fp = NULL;
/* Call-site gate for the per-instruction trace hooks (see recomp.h). Mirrors "s_fp != NULL":
 * the generated code checks this inline and only calls sr_begin_impl/sr_end_impl when set. */
int sr_trace_active = 0;
static uint32_t s_r[32], s_fi[32], s_vi[128], s_hi, s_lo, s_fcr31, s_pc, s_op;
static unsigned long long s_step = 0;

int sr_trace_open(const char *path, const char *target, uint32_t start_pc) {
    s_fp = fopen(path, "wb");
    if (!s_fp) return -1;
    fprintf(s_fp, "# psp-recomp trace v1 oracle=recomp target=%s start_pc=0x%08x\n",
            target ? target : "unknown", start_pc);
    s_step = 0;
    sr_trace_active = 1;
    return 0;
}

void sr_trace_close(void) {
    if (s_fp) { fflush(s_fp); fclose(s_fp); s_fp = NULL; }
    sr_trace_active = 0;
}

void sr_begin_impl(CpuState *s, uint32_t pc, uint32_t op) {
    if (!s_fp) return;
    memcpy(s_r, s->r, sizeof(s_r));
    memcpy(s_fi, s->fi, sizeof(s_fi));
    memcpy(s_vi, s->v, sizeof(s_vi));
    s_hi = s->hi; s_lo = s->lo; s_fcr31 = s->fcr31;
    s_pc = pc; s_op = op;
}

void sr_end_impl(CpuState *s, uint32_t mem_addr, int mem_size) {
    if (!s_fp) return;
    char line[4096];
    int n = snprintf(line, sizeof(line), "%llu pc=0x%08x op=0x%08x", s_step, s_pc, s_op);
    if (n < 0 || n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
    /* Emit the per-step register-write diff in the canonical TRACE_FORMAT.md order
     * (r1..r31, hi, lo, f0..f31, fcr31, v0..v127) so the recompiled-code trace is
     * byte-comparable with the reference interpreter's golden trace. Only registers
     * whose value changed since sr_begin_impl are listed. */
    for (int i = 1; i < 32; i++)
        if (s_r[i] != s->r[i])
            n += snprintf(line + n, sizeof(line) - (size_t)n, " r%d=0x%08x", i, s->r[i]);
    if (s_hi != s->hi)
        n += snprintf(line + n, sizeof(line) - (size_t)n, " hi=0x%08x", s->hi);
    if (s_lo != s->lo)
        n += snprintf(line + n, sizeof(line) - (size_t)n, " lo=0x%08x", s->lo);
    for (int i = 0; i < 32; i++)
        if (s_fi[i] != s->fi[i])
            n += snprintf(line + n, sizeof(line) - (size_t)n, " f%d=0x%08x", i, s->fi[i]);
    if (s_fcr31 != s->fcr31)
        n += snprintf(line + n, sizeof(line) - (size_t)n, " fcr31=0x%08x", s->fcr31);
    /* Compare and print the raw 32-bit register bits (the vi union view of the
     * float VFPU file): comparing the s_vi snapshot against the float view
     * converts through float, and passing a float to %x is undefined varargs
     * behavior. */
    for (int i = 0; i < 128; i++)
        if (s_vi[i] != s->vi[i])
            n += snprintf(line + n, sizeof(line) - (size_t)n, " v%d=0x%08x", i, s->vi[i]);
    /* Memory-write tokens (ascending by address), matching the interpreter. */
    if (mem_size == 1)
        n += snprintf(line + n, sizeof(line) - (size_t)n, " m8[0x%08x]=0x%02x",
                      mem_addr, MEM_R8(mem_addr));
    else if (mem_size == 2)
        n += snprintf(line + n, sizeof(line) - (size_t)n, " m16[0x%08x]=0x%04x",
                      mem_addr, MEM_R16(mem_addr));
    else if (mem_size == 4)
        n += snprintf(line + n, sizeof(line) - (size_t)n, " m32[0x%08x]=0x%08x",
                      mem_addr, MEM_R32(mem_addr));
    if (n < 0 || n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
    line[n] = '\n';
    fwrite(line, 1, n + 1, s_fp);
    s_step++;
    if (s_step > 50000000ULL) {
        fprintf(stderr, "sr_end: trace exceeded 50M steps\n");
        sr_trace_close();
        abort();
    }
}
