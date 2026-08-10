// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/*
 * White-box unit tests for the guest heap allocator's boundary-tag coalescing.
 * Standalone host executable, no game inputs required:
 *
 *   mingw32-make GAME_NAME=hst GAME_ELF=eboot.elf GAME_BASE=0 GAME_ENTRY=0 heap-selftest
 *
 * The harness #includes recomp.c so the allocator statics (s_heap_bump_ptr,
 * s_heap_free_list, the header/free bitmaps) and sr_heap_stats() are directly
 * inspectable, and drives the same public entry points the generated code calls
 * (sr_newlib_malloc / sr_newlib_free / sr_newlib_realloc).
 *
 * The assertions encode the ALLOCATOR CONTRACT, not the current implementation:
 *   - a freed block is reusable, and the heap's block chain stays walkable
 *     (metadata_valid) after every operation;
 *   - freeing physically adjacent blocks MERGES them, in either order, so the
 *     largest contiguous free block grows to span them (this is what issue #122
 *     needed: ~27 MB free but a 730 KB largest block could not satisfy a 3.9 MB
 *     request);
 *   - after fragmenting the arena and then releasing everything, an allocation
 *     far larger than any individual freed block SUCCEEDS -- the #122 regression;
 *   - coalescing never merges across a LIVE block, even when that block's payload
 *     happens to contain a footer-shaped word (the merge validators must reject
 *     it);
 *   - a merged block still hands back clean (zeroed) payload, preserving free()'s
 *     existing guarantee across the absorbed metadata words;
 *   - randomized alloc/free churn leaves metadata valid and the free list finite.
 */

#include "recomp.c"   /* white-box: allocator statics + sr_heap_stats() visible */

#include <stdlib.h>
#include <string.h>

/* ---- stubs for runtime symbols recomp.c references -------------------------------- */

uint32_t g_sr_debug = 0;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
int g_sr_mem_watch_count = 0;
int g_sr_metadata_watch = 0;
uint32_t g_sr_mem_watch_context_pc = 0;
unsigned g_sr_mem_watch_context_limit = 0;
unsigned g_sr_mem_watch_context_count = 0;
int g_sr_mem_watch_context_fpr = -1;
uint32_t g_sr_mem_watch_context_fpr_value = 0;
uint32_t g_sr_store_context_pc = 0;
unsigned g_sr_store_context_count = 0;
unsigned g_sr_store_context_limit = 0;
int g_sr_store_context_mem_gpr = -1;
uint32_t g_sr_store_context_mem_offset = 0;
unsigned g_sr_store_context_mem_words = 0;
int g_sr_last_writer_enabled = 0;
void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t val, uint32_t pc) {
    (void)addr; (void)width; (void)val; (void)pc;
}
CpuState *s_cpu = NULL;
int sr_sched_on = 0;
atomic_int_least32_t sr_timeslice;

uint32_t sched_current_uid(void) { return 0u; }
void sched_exit_current(int32_t status) { (void)status; }
void sched_exit_current_delete(int32_t status) { (void)status; }
uint32_t sched_start_thread(uint32_t uid, uint32_t arglen, uint32_t argp) { (void)uid; (void)arglen; (void)argp; return 0; }
uint32_t sched_terminate_thread(uint32_t uid) { (void)uid; return 0; }
uint32_t sched_delete_thread(uint32_t uid) { (void)uid; return 0; }
uint32_t sched_thread_wakeup(uint32_t uid) { (void)uid; return 0; }
void sched_set_current_join_target(uint32_t uid) { (void)uid; }
void sched_clear_current_join_target(void) {}
int sched_take_current_join_result(uint32_t uid, uint32_t *result_out) { (void)uid; (void)result_out; return 0; }
uint32_t sr_get_ge_status(void) { return 0u; }
uint32_t sr_hle_resolve_late_import(uint32_t nid) { (void)nid; return 0u; }
uint32_t sr_syscall(CpuState *s, uint32_t nid) { (void)s; (void)nid; return 0u; }
void sr_yield(CpuState *s) { (void)s; }
int sr_vfpu_interp(CpuState *s, uint32_t op) { (void)s; (void)op; return 0; }
uint64_t SDL_GetTicksNS(void) { return 0u; }

/* ---- harness ---------------------------------------------------------------------- */

static int g_failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } \
} while (0)

static void heap_reset(void) {
    uint32_t used = s_heap_bump_ptr - SR_HEAP_BASE;
    if (used) memset(SR_HOST(SR_HEAP_BASE), 0, used);
    s_heap_bump_ptr = SR_HEAP_BASE;
    s_heap_free_list = 0u;
    s_heap_fail_count = 0u;
    memset(s_heap_header_bits, 0, sizeof s_heap_header_bits);
    memset(s_heap_free_bits, 0, sizeof s_heap_free_bits);
    memset(s_heap_watch_reported_bits, 0, sizeof s_heap_watch_reported_bits);
}

/* Number of free blocks currently in the singly-linked list (bounded walk). */
static uint32_t free_list_len(void) {
    uint32_t n = 0, cur = s_heap_free_list;
    while (cur != 0u && n < 1000000u) { cur = MEM_R32(cur + 0u); n++; }
    return n;
}

/* ---- tests ------------------------------------------------------------------------ */

static void test_alloc_free_reuse(void) {
    heap_reset();
    uint32_t a = sr_newlib_malloc(64u, 0u);
    CHECK(a != 0u, "first malloc failed");
    SrHeapStats st = sr_heap_stats();
    CHECK(st.metadata_valid, "metadata invalid after malloc");
    sr_newlib_free(a, 0u);
    st = sr_heap_stats();
    CHECK(st.metadata_valid, "metadata invalid after free");
    uint32_t b = sr_newlib_malloc(64u, 0u);
    CHECK(b == a, "freed block was not reused (a=0x%08x b=0x%08x)", a, b);
}

static void test_forward_coalesce(void) {
    heap_reset();
    /* Three adjacent blocks; free the FIRST then the SECOND so the second free
     * absorbs its already-free successor (forward merge). */
    uint32_t a = sr_newlib_malloc(4096u, 0u);
    uint32_t b = sr_newlib_malloc(4096u, 0u);
    uint32_t c = sr_newlib_malloc(4096u, 0u);
    CHECK(a && b && c, "setup mallocs failed");
    sr_newlib_free(b, 0u);
    sr_newlib_free(a, 0u);
    SrHeapStats st = sr_heap_stats();
    CHECK(st.metadata_valid, "metadata invalid after coalescing frees");
    CHECK(st.free_blocks == 1u, "expected 1 merged free block, got %u", st.free_blocks);
    CHECK(st.largest_free >= 8192u,
          "largest free 0x%x should span both freed blocks", st.largest_free);
    (void)c;
}

static void test_backward_coalesce(void) {
    heap_reset();
    uint32_t a = sr_newlib_malloc(4096u, 0u);
    uint32_t b = sr_newlib_malloc(4096u, 0u);
    uint32_t c = sr_newlib_malloc(4096u, 0u);
    CHECK(a && b && c, "setup mallocs failed");
    /* Free in address order so each free folds into its free PREDECESSOR. */
    sr_newlib_free(a, 0u);
    sr_newlib_free(b, 0u);
    SrHeapStats st = sr_heap_stats();
    CHECK(st.metadata_valid, "metadata invalid after backward coalesce");
    CHECK(st.free_blocks == 1u, "expected 1 merged free block, got %u", st.free_blocks);
    CHECK(st.largest_free >= 8192u,
          "largest free 0x%x should span both freed blocks", st.largest_free);
    (void)c;
}

/* The #122 regression: fragment the arena into many small blocks, release them
 * all, then demand a block far larger than any individual one. Without
 * coalescing this fails despite ample total free bytes. */
static void test_fragmentation_then_large_alloc(void) {
    heap_reset();
    enum { N = 512, SZ = 8192u };
    uint32_t p[N];
    for (int i = 0; i < N; i++) {
        p[i] = sr_newlib_malloc(SZ, 0u);
        CHECK(p[i] != 0u, "setup malloc %d failed", i);
    }
    /* Free every other block first: maximum fragmentation, no two freed blocks
     * adjacent, so nothing can merge yet. */
    for (int i = 0; i < N; i += 2) sr_newlib_free(p[i], 0u);
    SrHeapStats frag = sr_heap_stats();
    CHECK(frag.metadata_valid, "metadata invalid while fragmented");
    CHECK(frag.largest_free < 2u * SZ,
          "checkerboard free should not yield a large block (got 0x%x)", frag.largest_free);
    /* Now release the rest: every hole becomes adjacent and must merge. */
    for (int i = 1; i < N; i += 2) sr_newlib_free(p[i], 0u);
    SrHeapStats st = sr_heap_stats();
    CHECK(st.metadata_valid, "metadata invalid after full release");
    CHECK(st.free_blocks <= 2u,
          "full release should collapse to ~1 free block, got %u", st.free_blocks);
    uint32_t big_bytes = (uint32_t)N * SZ / 2u;   /* far larger than any single block */
    uint32_t big = sr_newlib_malloc(big_bytes, 0u);
    CHECK(big != 0u,
          "large alloc of 0x%x failed after coalescing (largest_free=0x%x, free=%u blocks)",
          big_bytes, st.largest_free, st.free_blocks);
    if (big) sr_newlib_free(big, 0u);
}

/* Coalescing must never merge across a LIVE block, even if that block's payload
 * contains a word that looks exactly like a valid boundary-tag footer. */
static void test_no_merge_across_live_block(void) {
    heap_reset();
    uint32_t a = sr_newlib_malloc(4096u, 0u);
    uint32_t live = sr_newlib_malloc(4096u, 0u);
    uint32_t c = sr_newlib_malloc(4096u, 0u);
    CHECK(a && live && c, "setup mallocs failed");
    uint32_t live_hdr = live - 8u;
    uint32_t live_size = MEM_R32(live_hdr + 4u) & ~1u;
    sr_newlib_free(a, 0u);
    /* Forge a footer in the live block's last payload word claiming the live
     * block is a free predecessor of c. The free-bit validator must reject it. */
    MEM_W32(live_hdr + live_size - 4u, live_size);
    sr_newlib_free(c, 0u);
    SrHeapStats st = sr_heap_stats();
    CHECK(st.metadata_valid, "metadata invalid after forged-footer free");
    CHECK(st.allocated_blocks >= 1u, "the live block must still be allocated");
    CHECK(MEM_R32(live_hdr + 4u) == (live_size | 1u),
          "live block header was corrupted by a bad merge (0x%08x)",
          MEM_R32(live_hdr + 4u));
    /* a and c are not adjacent (live sits between), so they must stay separate. */
    CHECK(st.free_blocks == 2u, "expected 2 separate free blocks, got %u", st.free_blocks);
}

/* A merged block must still hand back zeroed payload: the absorbed header and
 * footer words are interior after a merge and must not leak stale values. */
static void test_merged_payload_is_clean(void) {
    heap_reset();
    uint32_t a = sr_newlib_malloc(4096u, 0u);
    uint32_t b = sr_newlib_malloc(4096u, 0u);
    CHECK(a && b, "setup mallocs failed");
    for (uint32_t off = 0; off < 4096u; off += 4u) {
        MEM_W32(a + off, 0xA5A5A5A5u);
        MEM_W32(b + off, 0x5A5A5A5Au);
    }
    sr_newlib_free(a, 0u);
    sr_newlib_free(b, 0u);   /* backward merge: a absorbs b */
    uint32_t big = sr_newlib_malloc(8000u, 0u);
    CHECK(big != 0u, "merged block should satisfy 8000 bytes");
    if (big) {
        int dirty = 0;
        for (uint32_t off = 0; off < 8000u; off += 4u)
            if (MEM_R32(big + off) != 0u) dirty++;
        CHECK(dirty == 0, "%d non-zero words in merged payload (stale metadata leaked)", dirty);
    }
}

/* Exact-fit reuse must not hand the free block's boundary-tag footer back to the
 * guest. A free block carries its size at hdr+size-4, which becomes ordinary
 * payload once the block is allocated again. When the request fits exactly the
 * block is NOT split, so nothing overwrites that word -- without an explicit
 * clear, the guest's fresh allocation ends with the block size instead of zero.
 *
 * The size is chosen so the footer lands on the last word of the REQUESTED
 * payload rather than in 16-byte rounding slack: alloc = round_up(size+8, 16), so
 * any size == 8 (mod 16) gives alloc == size+8 and puts the footer at payload
 * offset size-4. The sweep below then covers the whole block anyway, so the slack
 * is checked too.
 *
 * A partial remainder cannot occur today -- every allocation size and every free
 * block size is a multiple of 16, so block_size - alloc is either 0 or >= 16, and
 * malloc's `remainder >= 16` branch is exact-fit-or-split with no sliver in
 * between. The whole-block sweep is what would catch a future size class that
 * broke that invariant. */
static void test_exact_fit_reuse_payload_is_clean(void) {
    heap_reset();
    const uint32_t SZ = 4104u;   /* == 8 (mod 16) */
    uint32_t a = sr_newlib_malloc(SZ, 0u);
    CHECK(a != 0u, "setup malloc failed");
    if (!a) return;
    uint32_t hdr = a - 8u;
    uint32_t block_size = MEM_R32(hdr + 4u) & ~1u;
    CHECK(block_size == SZ + 8u, "expected exact-fit block 0x%x, got 0x%x", SZ + 8u, block_size);

    for (uint32_t off = 0; off < SZ; off += 4u) MEM_W32(a + off, 0xDEADBEEFu);
    sr_newlib_free(a, 0u);

    uint32_t b = sr_newlib_malloc(SZ, 0u);
    CHECK(b == a, "exact-fit reuse expected (a=0x%08x b=0x%08x)", a, b);
    if (!b) return;

    /* The requested payload, ending on the word the footer occupies. */
    CHECK(MEM_R32(b + SZ - 4u) == 0u,
          "last requested word leaked the boundary tag: 0x%08x (block size 0x%x)",
          MEM_R32(b + SZ - 4u), block_size);

    /* And the whole block, including the rounding slack past the request. */
    int dirty = 0;
    uint32_t rehdr = b - 8u;
    uint32_t resize = MEM_R32(rehdr + 4u) & ~1u;
    for (uint32_t addr = b; addr < rehdr + resize; addr += 4u)
        if (MEM_R32(addr) != 0u) dirty++;
    CHECK(dirty == 0, "%d non-zero words in exact-fit reused payload", dirty);
}

/* The same guarantee when the reused block was produced by a MERGE rather than by
 * a single free: the merged block's footer sits at the far end of the combined
 * span, so an exact-fit request for the whole merged size exposes it. */
static void test_exact_fit_after_merge_is_clean(void) {
    heap_reset();
    uint32_t a = sr_newlib_malloc(1016u, 0u);   /* two 1024-byte blocks */
    uint32_t b = sr_newlib_malloc(1016u, 0u);
    CHECK(a && b, "setup mallocs failed");
    if (!a || !b) return;
    for (uint32_t off = 0; off < 1016u; off += 4u) {
        MEM_W32(a + off, 0x11111111u);
        MEM_W32(b + off, 0x22222222u);
    }
    sr_newlib_free(a, 0u);
    sr_newlib_free(b, 0u);                      /* merged span = 2048 bytes */

    uint32_t merged = sr_newlib_malloc(2040u, 0u);   /* exact fit of the merged block */
    CHECK(merged == a, "merged block should be reused exactly (got 0x%08x)", merged);
    if (!merged) return;
    uint32_t hdr = merged - 8u;
    uint32_t size = MEM_R32(hdr + 4u) & ~1u;
    CHECK(size == 2048u, "expected a 2048-byte merged block, got 0x%x", size);
    int dirty = 0;
    for (uint32_t addr = merged; addr < hdr + size; addr += 4u)
        if (MEM_R32(addr) != 0u) dirty++;
    CHECK(dirty == 0, "%d non-zero words in exact-fit merged payload", dirty);
}

/* Randomized churn: metadata must stay valid and the free list must stay finite
 * (coalescing is what keeps it from growing without bound). */
static void test_random_churn(void) {
    heap_reset();
    enum { N = 256 };
    uint32_t p[N];
    memset(p, 0, sizeof p);
    uint32_t seed = 12345u;
    uint32_t peak_list = 0;
    for (int iter = 0; iter < 20000; iter++) {
        seed = seed * 1103515245u + 12345u;
        int idx = (int)((seed >> 16) % N);
        if (p[idx]) {
            sr_newlib_free(p[idx], 0u);
            p[idx] = 0u;
        } else {
            uint32_t sz = 16u + ((seed >> 8) % 8192u);
            p[idx] = sr_newlib_malloc(sz, 0u);
            CHECK(p[idx] != 0u, "churn malloc failed at iter %d (size %u)", iter, sz);
            if (p[idx] == 0u) break;
        }
        if ((iter & 0x3FF) == 0) {
            uint32_t len = free_list_len();
            if (len > peak_list) peak_list = len;
            SrHeapStats st = sr_heap_stats();
            CHECK(st.metadata_valid, "metadata invalid during churn at iter %d", iter);
            if (!st.metadata_valid) break;
        }
    }
    for (int i = 0; i < N; i++) if (p[i]) sr_newlib_free(p[i], 0u);
    SrHeapStats st = sr_heap_stats();
    CHECK(st.metadata_valid, "metadata invalid after churn drain");
    CHECK(st.free_blocks <= 2u,
          "draining all allocations should collapse the heap, got %u free blocks",
          st.free_blocks);
    fprintf(stderr, "  churn: peak free-list length %u, final free blocks %u\n",
            peak_list, st.free_blocks);
}

/* Corrupted guest metadata must be detected and quarantined rather than making
 * allocation search spin, follow an out-of-arena address, or accept overlapping
 * free blocks. The same guardrails also reject impossible uint32 allocation
 * sizes without changing a live allocation. */
static void test_metadata_guardrails(void) {
    heap_reset();
    uint32_t a = sr_newlib_malloc(64u, 0u);
    uint32_t live = sr_newlib_malloc(64u, 0u);
    CHECK(a && live, "out-of-arena setup mallocs failed");
    if (!a || !live) return;
    uint32_t a_hdr = a - 8u;
    sr_newlib_free(a, 0u);
    MEM_W32(a_hdr + 0u, SR_HEAP_END);  /* invalid free-list link */
    CHECK(!sr_heap_free_list_valid(), "out-of-arena free-list link was accepted");
    uint32_t after = sr_newlib_malloc(64u, 0u);
    CHECK(after != 0u && after != a, "out-of-arena free-list corruption was not quarantined");
    CHECK(s_heap_free_list == 0u, "invalid free-list was not cleared");
    CHECK(sr_heap_stats().metadata_valid, "valid block metadata was damaged by quarantine");

    heap_reset();
    a = sr_newlib_malloc(64u, 0u);
    live = sr_newlib_malloc(64u, 0u);
    CHECK(a && live, "cycle setup mallocs failed");
    if (!a || !live) return;
    a_hdr = a - 8u;
    sr_newlib_free(a, 0u);
    MEM_W32(a_hdr + 0u, a_hdr);  /* self-cycle */
    CHECK(!sr_heap_free_list_valid(), "cyclic free-list was accepted");
    after = sr_newlib_malloc(64u, 0u);
    CHECK(after != 0u && after != a, "cyclic free-list did not fall back safely");
    CHECK(s_heap_free_list == 0u, "cyclic free-list was not cleared");

    heap_reset();
    a = sr_newlib_malloc(64u, 0u);
    live = sr_newlib_malloc(64u, 0u);
    uint32_t c = sr_newlib_malloc(64u, 0u);
    CHECK(a && live && c, "overlap setup mallocs failed");
    if (!a || !live || !c) return;
    a_hdr = a - 8u;
    uint32_t c_hdr = c - 8u;
    sr_newlib_free(a, 0u);
    sr_newlib_free(c, 0u);
    uint32_t overlap_size = (c_hdr - a_hdr) + 16u;
    MEM_W32(a_hdr + 0u, c_hdr);
    MEM_W32(a_hdr + 4u, overlap_size);
    MEM_W32(a_hdr + overlap_size - 4u, overlap_size);
    s_heap_free_list = a_hdr;
    CHECK(!sr_heap_free_list_valid(), "overlapping free-list intervals were accepted");
    (void)sr_newlib_malloc(64u, 0u);  /* must quarantine without looping */
    CHECK(s_heap_free_list == 0u, "overlapping free-list was not cleared");
    CHECK(!sr_heap_stats().metadata_valid, "overlapping block metadata was not reported");

    heap_reset();
    a = sr_newlib_malloc(64u, 0u);
    CHECK(a != 0u, "overflow setup malloc failed");
    if (!a) return;
    uint32_t a_size = MEM_R32(a - 8u + 4u);
    CHECK(sr_newlib_malloc(0xffffffffu, 0u) == 0u,
          "uint32 allocation-size overflow was not rejected");
    CHECK(sr_newlib_realloc(a, 0xffffffffu, 0u) == 0u,
          "realloc size overflow was not rejected");
    CHECK(MEM_R32(a - 8u + 4u) == a_size,
          "overflowing realloc changed the live allocation");
    CHECK(sr_heap_stats().metadata_valid, "size-overflow guard damaged metadata");

    heap_reset();
    a = sr_newlib_malloc(64u, 0u);
    CHECK(a != 0u, "out-of-arena metadata setup failed");
    if (!a) return;
    MEM_W32(a - 8u + 4u, SR_HEAP_END);
    CHECK(!sr_heap_stats().metadata_valid,
          "out-of-arena block size was not reported by metadata validation");
}

static void test_foreign_and_interior_free_is_noop(void) {
    heap_reset();
    uint32_t a = sr_newlib_malloc(128u, 0u);
    uint32_t b = sr_newlib_malloc(64u, 0u);
    CHECK(a && b, "foreign-free setup mallocs failed");
    if (!a || !b) return;
    uint32_t a_sizeflags = MEM_R32(a - 8u + 4u);

    sr_newlib_free(a + 4u, 0u);             /* interior payload pointer */
    sr_newlib_free(1u, 0u);                 /* low foreign value */
    sr_newlib_free(SR_HEAP_END + 8u, 0u);   /* out-of-arena pointer */
    CHECK(MEM_R32(a - 8u + 4u) == a_sizeflags,
          "foreign/interior free changed the owned block header");
    CHECK(sr_heap_stats().metadata_valid,
          "foreign/interior free damaged allocator metadata");

    sr_newlib_free(a, 0u);
    sr_newlib_free(b, 0u);
    CHECK(sr_heap_stats().metadata_valid,
          "valid frees failed after foreign/interior free rejection");
}

static void test_adjacent_free_successor_realloc_growth(void) {
    heap_reset();
    /* Allocate three adjacent blocks A, B, C */
    uint32_t a = sr_newlib_malloc(64u, 0u);
    uint32_t b = sr_newlib_malloc(128u, 0u);
    uint32_t c = sr_newlib_malloc(64u, 0u);
    CHECK(a != 0u && b != 0u && c != 0u, "initial allocations failed");

    /* Store known pattern in A */
    memset(SR_HOST(a), 0x55, 64);

    /* Free B so that the space immediately following A is free */
    sr_newlib_free(b, 0u);

    /* Realloc A to expand into B's space (64 -> 128 bytes payload).
     * Since B is free and contiguous, realloc MUST expand in-place and return original pointer A. */
    uint32_t a_expanded = sr_newlib_realloc(a, 128u, 0u);
    CHECK(a_expanded == a, "realloc into adjacent free successor failed pointer identity assertion (got %x, expected %x)", a_expanded, a);

    /* Verify payload data was preserved */
    uint8_t *pat = (uint8_t *)SR_HOST(a_expanded);
    for (int i = 0; i < 64; i++) {
        CHECK(pat[i] == 0x55, "data corrupted at byte %d during adjacent realloc growth", i);
    }

    SrHeapStats st = sr_heap_stats();
    CHECK(st.metadata_valid, "metadata invalid after adjacent free successor realloc");

    /* Free A and C */
    sr_newlib_free(a_expanded, 0u);
    sr_newlib_free(c, 0u);
    st = sr_heap_stats();
    CHECK(st.metadata_valid, "metadata invalid after freeing expanded realloc blocks");
}

static void test_adjacent_free_successor_realloc_exact_consume(void) {
    heap_reset();
    /* Allocate three adjacent blocks A, B, C.
     * A: 64B payload -> 80B total block.
     * B: 128B payload -> 144B total block.
     * C: 64B payload -> 80B total block. */
    uint32_t a = sr_newlib_malloc(64u, 0u);
    uint32_t b = sr_newlib_malloc(128u, 0u);
    uint32_t c = sr_newlib_malloc(64u, 0u);
    CHECK(a != 0u && b != 0u && c != 0u, "initial allocations failed");

    memset(SR_HOST(a), 0x77, 64);

    /* Free B so space following A is free (144B free block). */
    sr_newlib_free(b, 0u);

    /* Realloc A to exactly consume all 224B of combined A+B block (216B payload).
     * Since 216B payload + 8B header = 224B, combined == wanted_total.
     * No trailing free remainder should be created. */
    uint32_t a_expanded = sr_newlib_realloc(a, 216u, 0u);
    CHECK(a_expanded == a, "exact consume realloc failed pointer identity assertion (got %x, expected %x)", a_expanded, a);

    /* Verify payload integrity */
    uint8_t *pat = (uint8_t *)SR_HOST(a_expanded);
    for (int i = 0; i < 64; i++) {
        CHECK(pat[i] == 0x77, "data corrupted at byte %d during exact-consume realloc", i);
    }

    SrHeapStats st = sr_heap_stats();
    CHECK(st.metadata_valid, "metadata invalid after exact-consume realloc");

    /* Free expanded block and remaining C block */
    sr_newlib_free(a_expanded, 0u);
    sr_newlib_free(c, 0u);
    st = sr_heap_stats();
    CHECK(st.metadata_valid, "metadata invalid after draining exact-consume heap");
}

static void test_adjacent_allocated_successor_realloc_fallback(void) {
    heap_reset();
    /* Allocate three adjacent blocks A, B, C.
     * All three remain allocated so B is a live (non-free) successor to A. */
    uint32_t a = sr_newlib_malloc(64u, 0u);
    uint32_t b = sr_newlib_malloc(128u, 0u);
    uint32_t c = sr_newlib_malloc(64u, 0u);
    CHECK(a != 0u && b != 0u && c != 0u, "initial allocations failed");

    memset(SR_HOST(a), 0x33, 64);
    memset(SR_HOST(b), 0x88, 128);

    /* Attempt to realloc A to 256B.
     * Since B is live/allocated, A cannot consume B and must fall back to
     * allocating a new block, copying A's data, and freeing old A. */
    uint32_t a_new = sr_newlib_realloc(a, 256u, 0u);
    CHECK(a_new != 0u && a_new != a, "realloc with live successor failed to allocate fallback block (a_new=%x, a=%x)", a_new, a);

    /* Verify A's data was copied to a_new */
    uint8_t *pat_a = (uint8_t *)SR_HOST(a_new);
    for (int i = 0; i < 64; i++) {
        CHECK(pat_a[i] == 0x33, "data in fallback realloc block corrupted at byte %d", i);
    }

    /* Verify live successor B's data remains untouched */
    uint8_t *pat_b = (uint8_t *)SR_HOST(b);
    for (int i = 0; i < 128; i++) {
        CHECK(pat_b[i] == 0x88, "live successor B payload corrupted at byte %d during A fallback realloc", i);
    }

    SrHeapStats st = sr_heap_stats();
    CHECK(st.metadata_valid, "metadata invalid after fallback realloc");

    /* Free all blocks */
    sr_newlib_free(a_new, 0u);
    sr_newlib_free(b, 0u);
    sr_newlib_free(c, 0u);
    st = sr_heap_stats();
    CHECK(st.metadata_valid, "metadata invalid after draining fallback heap");
}

int main(void) {
    /* 64 MB host arena. g_mem maps guest 0x08000000 -> arena[0], so the heap
     * region [0x0a000008,0x0c000000) lands inside arena[0x02000008,0x04000000). */
    uint8_t *arena = (uint8_t *)calloc(0x04000000u, 1u);
    if (!arena) { fprintf(stderr, "heap selftest: arena allocation failed\n"); return 1; }
    g_mem = arena;

    test_alloc_free_reuse();
    test_forward_coalesce();
    test_backward_coalesce();
    test_fragmentation_then_large_alloc();
    test_no_merge_across_live_block();
    test_merged_payload_is_clean();
    test_exact_fit_reuse_payload_is_clean();
    test_exact_fit_after_merge_is_clean();
    test_random_churn();
    test_metadata_guardrails();
    test_foreign_and_interior_free_is_noop();
    test_adjacent_free_successor_realloc_growth();
    test_adjacent_free_successor_realloc_exact_consume();
    test_adjacent_allocated_successor_realloc_fallback();

    if (g_failures) {
        fprintf(stderr, "heap selftest: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "heap selftest: OK\n");
    return 0;
}
