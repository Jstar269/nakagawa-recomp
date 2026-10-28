// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * guestmem_selftest.c - behavioral proof of the guest-memory bounds contract
 * (issue #15, and the #76 "execute, don't grep" testing philosophy).
 *
 * Compiles standalone against src/rt/recomp.h and executes real assertions on the
 * pure bounds/arithmetic helpers:
 *   - sr_inrange / sr_inrange_n      (existing scalar-access bounds)
 *   - sr_guest_span_readable/writable (bulk/parser span validation)
 *   - sr_guest_span_prefix (bounded partial-tail arithmetic)
 *   - sr_size_add_ok / sr_size_mul_ok (overflow-safe size arithmetic)
 *   - sr_guest_rect_readable/writable (pitched whole-rectangle validation)
 *   - sr_native_memmove/memset through the swizzled VRAM mirrors (#1049)
 *   - sr_guest_span_writable_any_view + sr_guest_copy_out/fill, the movie-frame
 *     write primitives, piece by piece through the swizzled mirrors (#1049)
 *
 * The core guarantee is overflow safety: a (base, size) pair whose naive sum wraps
 * uint32_t must be REJECTED, never accepted. A deterministic differential fuzz
 * compares each helper against a 64-bit-correct reference over the full 32-bit
 * range, so an overflow bug can't hide behind hand-picked cases.
 */

#include "recomp.h"

#include <stdlib.h>

/* Must match SR_PHYS()/the arena end in recomp.h. tools/test_guestmem_c.py asserts
 * these constants still match the header so the reference cannot silently drift. */
#define ARENA_END 0x0c000000ULL
#define PHYS_MASK 0x1fffffffu

static int g_fail = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            g_fail++;                                                       \
            printf("FAIL: ");                                               \
            printf(__VA_ARGS__);                                            \
            printf("  [%s:%d]\n", __FILE__, __LINE__);                      \
        }                                                                   \
    } while (0)

/* 64-bit-correct reference: is [phys(a), phys(a)+w) fully inside the arena? */
static int ref_span_ok(uint32_t a, uint32_t w) {
    uint64_t phys = (uint64_t)(a & PHYS_MASK);
    return (phys + (uint64_t)w) <= ARENA_END;
}

static void test_scalar_bounds(void) {
    /* First byte in / out of the arena. */
    CHECK(sr_inrange(0x08000000u), "RAM base must be in range");
    CHECK(sr_inrange(0x0bffffffu), "last arena byte must be in range");
    CHECK(!sr_inrange(0x0c000000u), "one past the arena must be out of range");
    /* kseg0/kseg1 mirrors alias down to the same physical arena. */
    CHECK(sr_inrange(0x88000000u), "kseg0 mirror of RAM base must be in range");
    CHECK(sr_inrange(0xa8000000u), "kseg1 mirror of RAM base must be in range");

    /* Width-aware: the LAST byte must land inside the arena, not just the first. */
    CHECK(sr_inrange_n(0x0bfffffcu, 4), "aligned 4-byte read ending at arena end is ok");
    CHECK(!sr_inrange_n(0x0bfffffeu, 4), "4-byte read whose tail passes arena end must fail");
    CHECK(!sr_inrange_n(0x0bffffffu, 2), "2-byte read straddling arena end must fail");

    /* A host pointer remains contiguous inside one swizzle block, but cannot span
     * a 32-byte or 64-byte remapping boundary. */
    CHECK(sr_guest_span_readable(0x04200100u, 4u),
          "a bulk span inside one 0x042 swizzle block remains contiguous");
    CHECK(!sr_guest_span_readable(0x0420003fu, 2u),
          "a bulk span crossing the 0x042 64-byte swizzle boundary must fail");
    CHECK(!sr_guest_span_writable(0x0460001fu, 2u),
          "a bulk span crossing the 0x046 32-byte swizzle boundary must fail");
    CHECK(!sr_guest_span_readable(0x04201fffu, 2u),
          "a bulk span crossing the 0x042 8 KiB swizzle boundary must fail");
}

static void test_overflow_safety(void) {
    /* The heart of #15: a base in-arena with a size so large that base+size wraps
     * uint32_t must still be rejected. A naive `phys + size <= END` would wrap and
     * wrongly accept. */
    CHECK(!sr_inrange_n(0x08000000u, 0xFFFFFFFFu), "huge width from in-arena base must be rejected");
    CHECK(!sr_guest_span_readable(0x08000000u, 0xFFFFFFFFu), "huge readable span must be rejected");
    CHECK(!sr_guest_span_writable(0x08000000u, 0xFFFFFFFFu), "huge writable span must be rejected");
    /* Exactly filling the arena from the base is the largest valid span. */
    CHECK(sr_guest_span_readable(0x08000000u, 0x04000000u), "span exactly to arena end is ok");
    CHECK(!sr_guest_span_readable(0x08000000u, 0x04000001u), "span one past arena end must fail");
    CHECK(sr_guest_span_prefix(0x0bfffff0u, 16u) == 16u,
          "prefix ending at the arena boundary must be complete");
    CHECK(sr_guest_span_prefix(0x0bfffff0u, 17u) == 16u,
          "prefix arithmetic must retain a one-byte valid-tail boundary");
    CHECK(sr_guest_span_prefix(0x0c000000u, 1u) == 0u,
          "prefix from one past the arena must be empty");
    CHECK(sr_guest_span_prefix(0x08000000u, 0xFFFFFFFFu) == 0x04000000u,
          "prefix arithmetic must remain overflow-safe for huge requests");
}

static void test_zero_size_span(void) {
    /* A zero-size span touches nothing and is always valid, even from an address
     * that is itself out of range (the base is never dereferenced). */
    CHECK(sr_guest_span_readable(0x08000000u, 0), "zero-size span at a valid base is ok");
    CHECK(sr_guest_span_readable(0xdeadbeefu, 0), "zero-size span at an invalid base is still ok");
    CHECK(sr_guest_span_writable(0xffffffffu, 0), "zero-size writable span is always ok");
}

static void test_checked_arith(void) {
    uint32_t out = 0xAAAAAAAAu;
    CHECK(sr_size_add_ok(10u, 20u, &out) && out == 30u, "10+20 must succeed as 30");
    CHECK(sr_size_add_ok(0xFFFFFFFFu, 0u, &out) && out == 0xFFFFFFFFu, "max+0 must succeed");
    out = 0x5555u;
    CHECK(!sr_size_add_ok(0xFFFFFFFFu, 1u, &out), "max+1 must report overflow");
    CHECK(out == 0x5555u, "add overflow must leave *out unmodified");

    out = 0xAAAAAAAAu;
    CHECK(sr_size_mul_ok(0u, 0xFFFFFFFFu, &out) && out == 0u, "0*max must succeed as 0");
    CHECK(sr_size_mul_ok(0x10000u, 0x10000u, NULL) == 0, "0x10000*0x10000 must overflow");
    out = 0x1234u;
    CHECK(!sr_size_mul_ok(0x80000000u, 3u, &out), "0x80000000*3 must report overflow");
    CHECK(out == 0x1234u, "mul overflow must leave *out unmodified");
    CHECK(sr_size_mul_ok(0xFFFFu, 0x10001u, &out) && out == 0xFFFFFFFFu, "0xFFFF*0x10001 == max, no overflow");
}

static void test_rect_bounds(void) {
    SrGuestRectSpan span;
    CHECK(sr_guest_rect_readable(0x08001000u, 3u, 2u, 8u, 4u, 3u, 2u, &span),
          "ordinary pitched rectangle must be readable");
    CHECK(span.first == 0x08001026u && span.row_pitch == 16u &&
          span.row_bytes == 8u && span.total_bytes == 40u,
          "rectangle geometry must resolve the exact first byte, pitch, row, and extent");

    CHECK(sr_guest_rect_writable(0x0bffeff0u, 3u, 0u, 1024u, 1u, 2u, 4u, &span) &&
          span.first == 0x0bffeffcu && span.total_bytes == 4100u,
          "a later row ending exactly at the arena boundary must be valid");
    CHECK(!sr_guest_rect_writable(0x0bfff000u, 1u, 0u, 1024u, 1u, 2u, 4u, &span),
          "a valid first row with a later row outside the arena must be rejected");

    CHECK(sr_guest_rect_readable(0x08002000u, 0u, 0u, 8u, 16u, 2u, 2u, &span) &&
          span.row_pitch == 16u && span.row_bytes == 32u && span.total_bytes == 48u,
          "overlapping rows remain a valid memory shape when pitch is narrower than a row");
    CHECK(sr_guest_rect_readable(0x88001000u, 0u, 0u, 8u, 1u, 1u, 4u, &span),
          "a valid kseg alias must resolve through the shared physical arena");

    CHECK(sr_guest_rect_readable(0xdeadbeefu, 0xffffffffu, 0xffffffffu,
                                 0xffffffffu, 0u, 9u, 4u, &span) &&
          span.total_bytes == 0u,
          "zero-width rectangles touch nothing even with otherwise invalid geometry");
    CHECK(sr_guest_rect_writable(0xffffffffu, 0u, 0u, 1u, 1u, 0u, 4u, &span) &&
          span.total_bytes == 0u,
          "zero-row rectangles touch nothing even from an invalid base");
    CHECK(!sr_guest_rect_readable(0x08000000u, 0u, 0u, 1u, 1u, 1u, 0u, &span),
          "a non-empty rectangle with zero bytes per pixel must be rejected");

    CHECK(!sr_guest_rect_readable(0xfffffff0u, 4u, 0u, 8u, 1u, 1u, 4u, &span),
          "base plus first-pixel offset must not wrap to a small guest address");
    CHECK(!sr_guest_rect_readable(0x08000000u, 0u, 0xffffffffu,
                                  0xffffffffu, 1u, 1u, 4u, &span),
          "origin multiplication overflow must be rejected");
    CHECK(!sr_guest_rect_readable(0x08000000u, 0u, 0u,
                                  0x80000000u, 1u, 3u, 4u, &span),
          "row-pitch or final-row multiplication overflow must be rejected");
    CHECK(!sr_guest_rect_readable(0x08000000u, 0u, 0u,
                                  1u, 0x80000000u, 1u, 4u, &span),
          "row-width multiplication overflow must be rejected");

    /* The two multiplication guards above are ordered, so a rectangle whose row
     * pitch is representable but whose FINAL-ROW offset is not must be rejected
     * on its own; likewise the final add. Without these the later branches are
     * never the branch that rejects. */
    CHECK(!sr_guest_rect_readable(0x08000000u, 0u, 0u,
                                  0x10000u, 1u, 0x10001u, 4u, &span),
          "final-row multiplication overflow must be rejected on a valid row pitch");
    CHECK(!sr_guest_rect_readable(0x08000000u, 0u, 0u,
                                  0x40000000u, 0x40000000u, 2u, 2u, &span),
          "final-row plus row-width addition overflow must be rejected");

    /* Tightest possible boundary pair. Every other assertion here still passes if
     * the accepted extent is widened by a single byte, so pin the exact end and
     * the first byte past it. The 16-bpp unaligned shape is reachable: TEXADDR0
     * carries no alignment mask, so a guest can place a linear texture at an odd
     * address whose final row ends one byte outside the arena. */
    CHECK(sr_guest_rect_readable(0x0bfffffcu, 0u, 0u, 1u, 1u, 2u, 2u, &span) &&
          span.total_bytes == 4u,
          "a rectangle whose last byte is the arena's last byte must be accepted");
    CHECK(!sr_guest_rect_readable(0x0bfffffdu, 0u, 0u, 1u, 1u, 2u, 2u, &span),
          "a rectangle reaching exactly one byte past the arena must be rejected");
    CHECK(!sr_guest_rect_writable(0x0bfffffdu, 0u, 0u, 1u, 1u, 2u, 2u, &span),
          "one byte past the arena must be rejected for writes as well as reads");
}

/* ---- Bulk CPU copies through the swizzled VRAM mirrors (#1049) ----------------
 * The generated stubs that replace a title's own memmove/memset call
 * sr_native_memmove/sr_native_memset (#1170); sceKernelMemcpy/Memset share the
 * same helpers. A CPU copy through a mirror must land where the same scalar
 * stores would. The expected physical offsets below come from the PSP-3000
 * measurement (docs/HARDWARE_ORACLE.md, PSP-VRAM-MIRROR-001), written out here
 * independently of sr_vram_cpu_address(). */

uint8_t *g_mem;
int g_sr_heap_watch;
volatile int g_sr_vram_read_pending;
static int g_oor_calls;
static uint8_t *g_committed;   /* one flag per physical VRAM byte */

/* Every prepare and commit the write primitives publish, in order (#1049). */
#define PIECE_LOG_MAX 4096u
static uint32_t g_prepare_addr[PIECE_LOG_MAX], g_prepare_width[PIECE_LOG_MAX];
static uint32_t g_commit_addr[PIECE_LOG_MAX], g_commit_width[PIECE_LOG_MAX];
static unsigned g_prepare_n, g_commit_n;
static void reset_piece_log(void) { g_prepare_n = 0; g_commit_n = 0; }

void sr_oor(uint32_t a, uint32_t v, int store) {
    (void)a; (void)v; (void)store;
    g_oor_calls++;
}
void sr_heap_note_bulk_write(uint32_t addr, uint32_t width, uint32_t pc) {
    (void)addr; (void)width; (void)pc;
}
void sr_ge_vram_read_prepare(uint32_t physical_addr, uint32_t width) {
    (void)physical_addr; (void)width;
}
void sr_ge_vram_write_prepare(uint32_t physical_addr, uint32_t width) {
    if (g_prepare_n < PIECE_LOG_MAX) {
        g_prepare_addr[g_prepare_n] = physical_addr;
        g_prepare_width[g_prepare_n] = width;
    }
    g_prepare_n++;
}

#define VRAM_BYTES 0x00200000u

/* Physical VRAM offset reached through view `view` (0..3) at offset `o`. */
static uint32_t measured_vram_offset(uint32_t view, uint32_t o) {
    if (view == 1u) return o ^ 0x2040u;
    if (view == 3u) {
        uint32_t bits = (o >> 5) & 0x1Fu;          /* bits 5..9 */
        bits = ((bits << 1) | (bits >> 4)) & 0x1Fu; /* rotate left one: bit 9 -> bit 5 */
        return ((o & ~0x3E0u) | (bits << 5)) ^ 0x2040u;
    }
    return o;
}

/* Commit reports carry the CPU address of a piece that one host pointer covers;
 * record the physical bytes it names so the test can require a commit for
 * every byte the copy stored. */
void sr_ge_vram_write_commit(uint32_t physical_addr, uint32_t width) {
    if (g_commit_n < PIECE_LOG_MAX) {
        g_commit_addr[g_commit_n] = physical_addr;
        g_commit_width[g_commit_n] = width;
    }
    g_commit_n++;
    uint32_t phys = physical_addr & PHYS_MASK;
    if (!g_committed || phys < 0x04000000u || phys >= 0x04800000u) return;
    uint32_t view = (phys >> 21) & 3u;
    uint32_t first = measured_vram_offset(view, phys & 0x001FFFFFu);
    for (uint32_t i = 0; i < width && first + i < VRAM_BYTES; i++)
        g_committed[first + i] = 1u;
}

static uint8_t *vram_phys(void) {
    return g_mem - 0x08000000u + 0x04000000u;
}

static void test_vram_mirror_bulk_copy(void) {
    static const uint32_t views[] = {
        0x04200000u, 0x04600000u, 0x44200000u, 0x44600000u,
    };
    /* 4 KiB starting mid-block: a partial first piece, then whole 32/64-byte
     * pieces, crossing the 8 KiB boundary at offset 0x12000 (bit 13). */
    const uint32_t offset = 0x00011F90u;
    const uint32_t size = 0x1000u;
    const uint32_t ram_src = 0x08100000u, ram_dst = 0x08200000u;
    uint8_t *vram = vram_phys();
    uint8_t *ram = g_mem;
    static uint8_t pattern[0x1000u];
    for (uint32_t i = 0; i < size; i++) pattern[i] = (uint8_t)(1u + (i * 7u + (i >> 8)) % 251u);

    for (size_t v = 0; v < sizeof views / sizeof views[0]; v++) {
        const uint32_t base = views[v];
        const uint32_t view = ((base & PHYS_MASK) >> 21) & 3u;
        const uint32_t guest = base + offset;

        memset(vram, 0, VRAM_BYTES);
        memset(g_committed, 0, VRAM_BYTES);
        memcpy(ram + (ram_src - 0x08000000u), pattern, size);
        memset(ram + (ram_dst - 0x08000000u), 0, size);
        g_oor_calls = 0;

        /* RAM -> mirror: each byte at its measured physical offset. */
        sr_native_memmove(guest, ram_src, size, 0u);
        CHECK(g_oor_calls == 0, "view %08x: memmove into the mirror was refused", (unsigned)base);
        uint32_t misplaced = 0, uncommitted = 0, stray = 0;
        for (uint32_t i = 0; i < size; i++) {
            uint32_t phys = measured_vram_offset(view, offset + i);
            if (vram[phys] != pattern[i]) misplaced++;
            if (!g_committed[phys]) uncommitted++;
        }
        for (uint32_t p = 0; p < VRAM_BYTES; p++)
            if (vram[p] != 0u) stray++;
        CHECK(misplaced == 0, "view %08x: %u of 4096 bytes not at the measured offset",
              (unsigned)base, (unsigned)misplaced);
        CHECK(uncommitted == 0, "view %08x: %u stored bytes were never committed",
              (unsigned)base, (unsigned)uncommitted);
        CHECK(stray == size, "view %08x: %u nonzero VRAM bytes, expected exactly 4096",
              (unsigned)base, (unsigned)stray);

        /* Mirror -> RAM: reading back through the same view returns the pattern. */
        sr_native_memmove(ram_dst, guest, size, 0u);
        CHECK(g_oor_calls == 0, "view %08x: memmove out of the mirror was refused", (unsigned)base);
        CHECK(memcmp(ram + (ram_dst - 0x08000000u), pattern, size) == 0,
              "view %08x: read back through the mirror differs", (unsigned)base);

        /* Fill through the mirror touches exactly the measured bytes. */
        memset(vram, 0, VRAM_BYTES);
        sr_native_memset(guest, 0xA5u, size, 0u);
        CHECK(g_oor_calls == 0, "view %08x: memset into the mirror was refused", (unsigned)base);
        misplaced = 0;
        stray = 0;
        for (uint32_t i = 0; i < size; i++)
            if (vram[measured_vram_offset(view, offset + i)] != 0xA5u) misplaced++;
        for (uint32_t p = 0; p < VRAM_BYTES; p++)
            if (vram[p] != 0u) stray++;
        CHECK(misplaced == 0 && stray == size,
              "view %08x: memset placed %u bytes wrongly, %u nonzero bytes",
              (unsigned)base, (unsigned)misplaced, (unsigned)stray);
    }

    /* The measured example: a store to 0x04200100 lands at physical 0x002140. */
    memset(vram, 0, VRAM_BYTES);
    sr_native_memset(0x04200100u, 0x5Au, 1u, 0u);
    CHECK(vram[0x2140u] == 0x5Au, "a store to 0x04200100 must land at physical 0x002140");

    /* Overlapping physical bytes: the base view copied onto the 0x042 view of
     * the same VRAM keeps memmove semantics (every source byte read first). */
    static uint8_t before[0x4000u], expect_vram[0x4000u], staged[0x1000u];
    const uint32_t window = 0x00010000u;
    for (uint32_t p = 0; p < sizeof before; p++) before[p] = (uint8_t)(p * 13u + 5u);
    memcpy(vram + window, before, sizeof before);
    memcpy(expect_vram, before, sizeof before);
    /* dst offsets 0x12120.. have bit 13 set, so the 0x042 view maps them back
     * onto physical 0x10160.., inside the source bytes still to be read. */
    const uint32_t src_off = window + 0x100u, dst_off = window + 0x2120u;
    for (uint32_t i = 0; i < size; i++) staged[i] = before[src_off + i - window];
    for (uint32_t i = 0; i < size; i++)
        expect_vram[measured_vram_offset(1u, dst_off + i) - window] = staged[i];
    g_oor_calls = 0;
    sr_native_memmove(0x04200000u + dst_off, 0x04000000u + src_off, size, 0u);
    CHECK(g_oor_calls == 0, "overlapping base-to-mirror memmove was refused");
    CHECK(memcmp(vram + window, expect_vram, sizeof expect_vram) == 0,
          "overlapping base-to-mirror memmove must read every source byte first");

    /* Arena bounds still fail closed without touching memory. */
    memset(vram, 0, VRAM_BYTES);
    g_oor_calls = 0;
    sr_native_memmove(0x0bfff800u, 0x04200000u, size, 0u);
    CHECK(g_oor_calls == 2, "a destination past the arena end must be refused");
    CHECK(ram[0x03fff800u] == 0u, "a refused copy must not store its first byte");
}

/* ---- Movie-frame write primitives through the swizzled mirrors (#1049) ------------
 * sr_guest_span_writable_any_view() + sr_guest_copy_out() / sr_guest_fill() are what the
 * movie frame writers (sceMpegAvcCsc, sceMpegAvcDecode, the H.264 guest targets and the
 * PSMF player) use. The measured offsets again come from measured_vram_offset(), not from
 * sr_vram_cpu_address(). */

/* Physical arena address a CPU address reaches, from the measured transforms. */
static uint32_t measured_phys(uint32_t guest) {
    uint32_t phys = guest & PHYS_MASK;
    if (phys < 0x04000000u || phys >= 0x04800000u) return phys;
    return 0x04000000u + measured_vram_offset((phys >> 21) & 3u, phys & 0x001FFFFFu);
}

static int span_in_one_block(uint32_t guest, uint32_t width) {
    const uint32_t first = measured_phys(guest);
    for (uint32_t i = 1; i < width; i++)
        if (measured_phys(guest + i) != first + i) return 0;
    return 1;
}

/* Check one write of `size` bytes at `guest` already performed (VRAM zero-initialised, the
 * stored byte for offset i being want(i)): placement, exact touched count, and the
 * prepare/commit pieces the write published. `blocks` is the independently derived number of
 * physically contiguous blocks the span covers. */
static void check_vram_write(const char *what, uint32_t guest, uint32_t size,
                             const uint8_t *want, uint32_t blocks) {
    const uint8_t *vram = vram_phys();
    uint32_t misplaced = 0, uncommitted = 0, stray = 0;
    for (uint32_t i = 0; i < size; i++) {
        const uint32_t off = measured_phys(guest + i) - 0x04000000u;
        if (vram[off] != want[i]) misplaced++;
        if (!g_committed[off]) uncommitted++;
    }
    for (uint32_t p = 0; p < VRAM_BYTES; p++)
        if (vram[p] != 0u) stray++;
    CHECK(misplaced == 0, "%s @%08x+%x: %u bytes not at the measured offset", what,
          (unsigned)guest, (unsigned)size, (unsigned)misplaced);
    CHECK(uncommitted == 0, "%s @%08x+%x: %u bytes never committed", what, (unsigned)guest,
          (unsigned)size, (unsigned)uncommitted);
    CHECK(stray == size, "%s @%08x+%x: %u nonzero VRAM bytes, expected %u", what,
          (unsigned)guest, (unsigned)size, (unsigned)stray, (unsigned)size);

    CHECK(g_commit_n == g_prepare_n && g_commit_n == blocks,
          "%s @%08x+%x: %u prepares and %u commits, expected %u pieces", what,
          (unsigned)guest, (unsigned)size, (unsigned)g_prepare_n, (unsigned)g_commit_n,
          (unsigned)blocks);
    uint32_t next = guest, covered = 0;
    for (unsigned i = 0; i < g_commit_n && i < PIECE_LOG_MAX; i++) {
        const uint32_t addr = g_commit_addr[i], width = g_commit_width[i];
        CHECK(addr == next, "%s @%08x+%x: piece %u starts at %08x, expected %08x", what,
              (unsigned)guest, (unsigned)size, i, (unsigned)addr, (unsigned)next);
        CHECK(width != 0u && span_in_one_block(addr, width),
              "%s @%08x+%x: piece %u (%08x, %u bytes) is not one physical block", what,
              (unsigned)guest, (unsigned)size, i, (unsigned)addr, (unsigned)width);
        CHECK(i < g_prepare_n && g_prepare_addr[i] == addr && g_prepare_width[i] == width,
              "%s @%08x+%x: piece %u was not prepared with the span it committed", what,
              (unsigned)guest, (unsigned)size, i);
        next += width;
        covered += width;
    }
    CHECK(covered == size, "%s @%08x+%x: pieces cover %u bytes", what, (unsigned)guest,
          (unsigned)size, (unsigned)covered);
}

/* Number of aligned `block`-byte blocks [guest, guest+size) touches. */
static uint32_t blocks_touched(uint32_t guest, uint32_t size, uint32_t block) {
    return ((guest + size - 1u) / block) - (guest / block) + 1u;
}

static void test_any_view_span_predicate(void) {
    /* Spans the refusing predicate rejects, which the any-view predicate accepts. */
    static const struct { uint32_t addr, size; const char *what; } accept[] = {
        { 0x04200000u, 0x20u,  "one 32-byte block, mirror 1" },
        { 0x04600010u, 0x40u,  "crosses a 32-byte block, mirror 3" },
        { 0x04201FF0u, 0x40u,  "crosses the 8 KiB XOR boundary, mirror 1" },
        { 0x04201FF0u, 0x3000u,"several 8 KiB XOR boundaries, mirror 1" },
        { 0x44600001u, 0x1001u,"unaligned start and end, uncached mirror 3" },
        { 0x041FFFF0u, 0x40u,  "base view into mirror 1" },
        { 0x045FFFF0u, 0x40u,  "mirror 2 into mirror 3" },
        { 0x047FFFF0u, 0x40u,  "mirror 3 into the arena above VRAM" },
        { 0x08000000u, 0x100u, "ordinary RAM" },
        { 0x0bfffffcu, 4u,     "the last arena word" },
        { 0x0c000000u, 0u,     "an empty span past the arena" },
    };
    for (size_t i = 0; i < sizeof accept / sizeof accept[0]; i++)
        CHECK(sr_guest_span_writable_any_view(accept[i].addr, accept[i].size),
              "any-view predicate refused %s (%08x+%x)", accept[i].what,
              (unsigned)accept[i].addr, (unsigned)accept[i].size);

    static const struct { uint32_t addr, size; const char *what; } refuse[] = {
        { 0x0bfffff0u, 0x20u,        "runs off the arena end" },
        { 0x0c000000u, 1u,           "starts past the arena" },
        { 0x04600000u, 0xFFFFFFFFu,  "size wraps the address space" },
        { 0xFFFFFFF0u, 0x20u,        "wraps through the physical mask" },
        { 0x04200000u, 0x08000000u,  "larger than the arena" },
    };
    for (size_t i = 0; i < sizeof refuse / sizeof refuse[0]; i++)
        CHECK(!sr_guest_span_writable_any_view(refuse[i].addr, refuse[i].size),
              "any-view predicate accepted %s (%08x+%x)", refuse[i].what,
              (unsigned)refuse[i].addr, (unsigned)refuse[i].size);

    /* The refusing predicate keeps refusing those mirror spans for every other caller. */
    CHECK(!sr_guest_span_writable(0x04600010u, 0x40u),
          "the pointer-forming predicate must still refuse a 0x046 span over a block boundary");
    CHECK(!sr_guest_span_writable(0x04201FF0u, 0x40u),
          "the pointer-forming predicate must still refuse a 0x042 span over the 8 KiB boundary");
    CHECK(sr_guest_span_writable(0x04200000u, 0x40u),
          "the pointer-forming predicate still accepts one 0x042 block");
}

static void test_vram_mirror_write_primitives(void) {
    static const uint32_t views[] = { 0x04200000u, 0x04600000u, 0x44200000u, 0x44600000u };
    static const struct { uint32_t offset, size; const char *what; } spans[] = {
        { 0x00000000u, 0x20u,   "one aligned block" },
        { 0x0000001Du, 0x05u,   "unaligned inside one 32-byte block" },
        { 0x0000001Du, 0x06u,   "unaligned across one 32-byte boundary" },
        { 0x0000003Fu, 0x02u,   "two bytes across a 64-byte boundary" },
        { 0x00001FF0u, 0x30u,   "across the 8 KiB XOR boundary" },
        { 0x00011F90u, 0x1000u, "4 KiB mid-block across the 8 KiB boundary" },
        { 0x00002005u, 0x3003u, "unaligned start and end across two XOR boundaries" },
        { 0x001FFFE3u, 0x1Du,   "ends at the end of a mirror window" },
    };
    uint8_t *vram = vram_phys();
    static uint8_t pattern[0x3003u];
    for (uint32_t i = 0; i < sizeof pattern; i++)
        pattern[i] = (uint8_t)(1u + (i * 11u + (i >> 7)) % 251u);
    g_sr_vram_read_pending = 1;

    for (size_t v = 0; v < sizeof views / sizeof views[0]; v++) {
        const uint32_t view = ((views[v] & PHYS_MASK) >> 21) & 3u;
        const uint32_t block = view == 3u ? 0x20u : 0x40u;
        for (size_t s = 0; s < sizeof spans / sizeof spans[0]; s++) {
            const uint32_t guest = views[v] + spans[s].offset, size = spans[s].size;

            /* copy_out */
            memset(vram, 0, VRAM_BYTES);
            memset(g_committed, 0, VRAM_BYTES);
            reset_piece_log();
            CHECK(sr_guest_copy_out(guest, pattern, size) == 1, "copy_out refused %s @%08x",
                  spans[s].what, (unsigned)guest);
            check_vram_write("copy_out", guest, size, pattern,
                             blocks_touched(guest & PHYS_MASK, size, block));

            /* fill */
            static uint8_t fill_want[0x3003u];
            memset(fill_want, 0xC3, sizeof fill_want);
            memset(vram, 0, VRAM_BYTES);
            memset(g_committed, 0, VRAM_BYTES);
            reset_piece_log();
            CHECK(sr_guest_fill(guest, 0xC3u, size) == 1, "fill refused %s @%08x",
                  spans[s].what, (unsigned)guest);
            check_vram_write("fill", guest, size, fill_want,
                             blocks_touched(guest & PHYS_MASK, size, block));
        }
    }

    /* A span that starts in the base view and runs into the 0x042 mirror: the first bytes are
     * linear, the rest follow the mirror transform. */
    {
        const uint32_t guest = 0x041FFFF0u, size = 0x50u;
        memset(vram, 0, VRAM_BYTES);
        memset(g_committed, 0, VRAM_BYTES);
        reset_piece_log();
        CHECK(sr_guest_copy_out(guest, pattern, size) == 1, "cross-view copy_out refused");
        check_vram_write("cross-view copy_out", guest, size, pattern,
                         1u + blocks_touched(0x04200000u, size - 0x10u, 0x40u));
    }

    /* Fail closed: out-of-range touches nothing and reports it. */
    memset(vram, 0, VRAM_BYTES);
    reset_piece_log();
    CHECK(sr_guest_copy_out(0x0bfffff0u, pattern, 0x20u) == 0, "copy_out past the arena end");
    CHECK(sr_guest_fill(0x0bfffff0u, 0x11u, 0x20u) == 0, "fill past the arena end");
    CHECK(sr_guest_copy_out(0x04600000u, pattern, 0xFFFFFFFFu) == 0, "copy_out with a wrapping size");
    CHECK(g_commit_n == 0u && g_prepare_n == 0u, "a refused write must not publish VRAM events");
    {
        uint32_t stray = 0;
        for (uint32_t p = 0; p < VRAM_BYTES; p++) if (vram[p]) stray++;
        CHECK(stray == 0u, "a refused write must not store a byte");
    }
    CHECK(sr_guest_copy_out(0x04600000u, pattern, 0u) == 1 && sr_guest_fill(0x04600000u, 1u, 0u) == 1,
          "an empty write succeeds without touching memory");
    g_sr_vram_read_pending = 0;
}

static void fuzz_differential(void) {
    /* Deterministic LCG; no host RNG, so the run is reproducible. Covers the full
     * 32-bit range for both base and size, hitting near-boundary and huge values. */
    uint32_t x = 0x12345678u;
    const int iters = 400000;
    int span_mismatch = 0, add_mismatch = 0, mul_mismatch = 0;
    for (int i = 0; i < iters; i++) {
        x = x * 1103515245u + 12345u;
        uint32_t a = x;
        x = x * 1103515245u + 12345u;
        uint32_t s = x;
        /* Bias a chunk of samples toward the boundary and toward huge sizes. */
        if ((i & 7) == 0) a = 0x08000000u + (x & 0x07ffffffu);
        if ((i & 3) == 0) s = 0xFFFFFF00u + (x & 0xFFu);

        if (s != 0u) {
            int got = sr_inrange_n(a, s);
            int want = ref_span_ok(a, s);
            if (got != want) span_mismatch++;
            int gotr = sr_guest_span_readable(a, s);
            int span_want = want && !sr_guest_span_hits_swizzled_vram_mirror(a, s);
            if (gotr != span_want) span_mismatch++;
        }

        uint32_t out = 0;
        int add_ok = sr_size_add_ok(a, s, &out);
        int add_ref = ((uint64_t)a + (uint64_t)s) <= 0xFFFFFFFFULL;
        if (add_ok != add_ref) add_mismatch++;
        if (add_ok && out != (uint32_t)(a + s)) add_mismatch++;

        int mul_ok = sr_size_mul_ok(a, s, &out);
        int mul_ref = ((uint64_t)a * (uint64_t)s) <= 0xFFFFFFFFULL;
        if (mul_ok != mul_ref) mul_mismatch++;
        if (mul_ok && out != (uint32_t)(a * s)) mul_mismatch++;
    }
    CHECK(span_mismatch == 0, "span check disagreed with 64-bit reference %d time(s)", span_mismatch);
    CHECK(add_mismatch == 0, "checked add disagreed with reference %d time(s)", add_mismatch);
    CHECK(mul_mismatch == 0, "checked mul disagreed with reference %d time(s)", mul_mismatch);
}

int main(void) {
    test_scalar_bounds();
    test_overflow_safety();
    test_zero_size_span();
    test_checked_arith();
    test_rect_bounds();
    fuzz_differential();
    uint8_t *arena = (uint8_t *)calloc((size_t)ARENA_END, 1);
    g_committed = (uint8_t *)calloc(VRAM_BYTES, 1);
    if (!arena || !g_committed) {
        printf("guestmem selftest: no guest arena\n");
        return 2;
    }
    g_mem = arena + 0x08000000u;
    test_vram_mirror_bulk_copy();
    test_any_view_span_predicate();
    test_vram_mirror_write_primitives();
    free(g_committed);
    free(arena);
    if (g_fail == 0) {
        printf("guestmem selftest: OK\n");
        return 0;
    }
    printf("guestmem selftest: %d FAILURE(S)\n", g_fail);
    return 1;
}
