// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Host-neutral regression for the TD-27 stale translated-code tracker
// (src/rt/stale_code.h/.c). A synthetic "translated block" is a few words of
// fake guest memory with registered translation-time expectations; the test
// overwrites those bytes and runs the invalidate-time check, exactly the
// shape the HLE cache handlers execute in production. The invalidation half
// additionally asserts the per-entry stale flag and the dispatch-side
// sr_stale_block_is_stale() query through a modeled AOT-vs-interpreter tier
// branch. No title data, no image, no guest input.
//
// Two modes, selected by the SR_STALE_DETECT environment gate itself:
//   gate off (unset): register + overwrite + check must stay SILENT (the
//                     opt-in contract: off by default, off the hot path).
//   gate on  (=1):    the full matrix below; exit code 0 means every
//                     invariant holds.

#include "stale_code.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int g_failed = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL L%d: ", __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); g_failed = 1; } \
} while (0)

#define FAKE_BASE 0x08800000u
#define FAKE_WORDS 64u

static uint32_t s_fake[FAKE_WORDS];

static int fake_read(uint32_t addr, uint32_t *word_out, void *ctx) {
    uint32_t index;
    (void)ctx;
    if ((addr & 3u) != 0u || addr < FAKE_BASE) {
        return 0;
    }
    index = (addr - FAKE_BASE) / 4u;
    if (index >= FAKE_WORDS) {
        return 0;
    }
    *word_out = s_fake[index];
    return 1;
}

static void fake_write(uint32_t addr, uint32_t word) {
    s_fake[(addr - FAKE_BASE) / 4u] = word;
}

/* Shared FNV-1a vectors with tools/codegen.py::stale_fnv1a. */
static void test_fnv_vectors(void) {
    static const uint32_t pair[2] = { 0x11111111u, 0x22222222u };
    static const uint32_t trio[3] = { 0x24020001u, 0x03e00008u, 0x00000000u };
    static const uint32_t one[1] = { 0x00000061u };
    static const uint32_t zero[1] = { 0x00000000u };
    CHECK(sr_stale_fnv1a(NULL, 5u) == 0x811c9dc5u, "fnv empty != basis");
    CHECK(sr_stale_fnv1a(zero, 1u) == 0x4b95f515u, "fnv [0] = 0x%08x", sr_stale_fnv1a(zero, 1u));
    CHECK(sr_stale_fnv1a(one, 1u) == 0xf5e1d3e4u, "fnv [0x61] = 0x%08x", sr_stale_fnv1a(one, 1u));
    CHECK(sr_stale_fnv1a(pair, 2u) == 0x12a02059u, "fnv pair = 0x%08x", sr_stale_fnv1a(pair, 2u));
    CHECK(sr_stale_fnv1a(trio, 3u) == 0x879f43e7u, "fnv trio = 0x%08x", sr_stale_fnv1a(trio, 3u));
}

#define BLOCK_A (FAKE_BASE + 0x00u)
#define WORD_B (FAKE_BASE + 0x08u)
#define BLOCK_C (FAKE_BASE + 0x10u)
#define WORD_W0 0x11111111u
#define WORD_W1 0x22222222u
#define WORD_W2 0x33333333u
#define PAIR_HASH 0x12a02059u

static void test_gate_off_is_silent(void) {
    uint32_t bad = 0xdeadbeefu;
    uint32_t exp = 0xdeadbeefu;
    uint32_t act = 0xdeadbeefu;
    int rc_range;
    int rc_all;
    CHECK(sr_stale_enabled() == 0, "gate must read disabled without SR_STALE_DETECT");
    sr_stale_reset();
    sr_stale_register_word(BLOCK_A + 4u, WORD_W1);
    fake_write(BLOCK_A + 4u, 0xdeadbeefu); /* overwrite, then "invalidate" */
    rc_range = sr_stale_check_range(BLOCK_A, 8u, fake_read, NULL, &bad, &exp, &act);
    rc_all = sr_stale_check_all(fake_read, NULL, &bad, &exp, &act);
    CHECK(rc_range == 0, "gate off: overwritten invalidate must stay silent (range)");
    CHECK(rc_all == 0, "gate off: overwritten invalidate must stay silent (all)");
    CHECK(bad == 0u && exp == 0u && act == 0u, "gate off: outputs must be zeroed");
    CHECK(sr_stale_block_is_stale(BLOCK_A + 4u) == 0,
          "gate off: query must stay clean so dispatch keeps the AOT path");
}

static void test_detector_matrix(void) {
    uint32_t bad = 0u;
    uint32_t exp = 0u;
    uint32_t act = 0u;
    uint32_t before;
    CHECK(sr_stale_enabled() == 1, "gate must read enabled with SR_STALE_DETECT=1");

    sr_stale_reset();
    CHECK(sr_stale_entry_count() == 0u, "reset must empty the table");
    sr_stale_register_word(BLOCK_A, WORD_W0);
    sr_stale_register_word(BLOCK_A + 4u, WORD_W1);
    sr_stale_register_word(WORD_B, WORD_W2);
    sr_stale_register_block(BLOCK_C, 2u, PAIR_HASH);
    CHECK(sr_stale_entry_count() == 4u, "entry count = %u, want 4", sr_stale_entry_count());
    /* Re-registration replaces instead of duplicating. */
    sr_stale_register_word(BLOCK_A, WORD_W0);
    CHECK(sr_stale_entry_count() == 4u, "re-register must not duplicate");

    /* Pristine fake memory: every invalidate is silent. */
    fake_write(BLOCK_A, WORD_W0);
    fake_write(BLOCK_A + 4u, WORD_W1);
    fake_write(WORD_B, WORD_W2);
    fake_write(BLOCK_C, WORD_W0);
    fake_write(BLOCK_C + 4u, WORD_W1);
    CHECK(sr_stale_check_range(BLOCK_A, 8u, fake_read, NULL, &bad, &exp, &act) == 0,
          "pristine range must be silent");
    CHECK(sr_stale_check_range(WORD_B, 4u, fake_read, NULL, &bad, &exp, &act) == 0,
          "pristine word must be silent");
    CHECK(sr_stale_check_all(fake_read, NULL, &bad, &exp, &act) == 0,
          "pristine check-all must be silent");
    CHECK(bad == 0u && exp == 0u && act == 0u, "clean outputs must be zeroed");

    /* Overwrite one translated word, then invalidate: the detector fires with
     * the block address, the first differing word, and expected vs actual. */
    fake_write(BLOCK_A + 4u, 0xdeadbeefu);
    bad = exp = act = 0u;
    CHECK(sr_stale_check_range(BLOCK_A, 8u, fake_read, NULL, &bad, &exp, &act) == 1,
          "overwritten invalidate must fire (range)");
    CHECK(bad == BLOCK_A + 4u && exp == WORD_W1 && act == 0xdeadbeefu,
          "fire must name off=0x%08x exp=0x%08x act=0x%08x", bad, exp, act);
    bad = exp = act = 0u;
    CHECK(sr_stale_check_all(fake_read, NULL, &bad, &exp, &act) == 1,
          "overwritten invalidate must fire (all)");
    CHECK(bad == BLOCK_A + 4u && exp == WORD_W1 && act == 0xdeadbeefu,
          "check-all must name the same word");

    /* Disjoint invalidates stay silent while another block is stale. */
    CHECK(sr_stale_check_range(BLOCK_C, 8u, fake_read, NULL, &bad, &exp, &act) == 0,
          "disjoint range must stay silent");
    CHECK(sr_stale_check_range(WORD_B, 4u, fake_read, NULL, &bad, &exp, &act) == 0,
          "disjoint word must stay silent");
    fake_write(BLOCK_A + 4u, WORD_W1);
    CHECK(sr_stale_check_range(BLOCK_A, 8u, fake_read, NULL, &bad, &exp, &act) == 0,
          "restored range must be silent again");

    /* Block-only region (no word entries): an interior overwrite fires at
     * hash granularity, naming the block and both hashes. */
    fake_write(BLOCK_C + 4u, 0x0bad0badu);
    bad = exp = act = 0u;
    CHECK(sr_stale_check_range(BLOCK_C, 8u, fake_read, NULL, &bad, &exp, &act) == 1,
          "interior overwrite must fire (block hash)");
    CHECK(bad == BLOCK_C && exp == PAIR_HASH && act != PAIR_HASH,
          "block fire must name block=0x%08x hash exp=0x%08x act=0x%08x", bad, exp, act);
    fake_write(BLOCK_C + 4u, WORD_W1);

    /* Unreadable records are skipped, never stale. */
    sr_stale_register_word(0x09000000u, 0x12345678u);
    CHECK(sr_stale_entry_count() == 5u, "entry count = %u, want 5", sr_stale_entry_count());
    CHECK(sr_stale_check_all(fake_read, NULL, &bad, &exp, &act) == 0,
          "unreadable record must be skipped");

    /* Malformed inputs are silent, never stale and never a crash. */
    CHECK(sr_stale_check_range(BLOCK_A, 0u, fake_read, NULL, &bad, &exp, &act) == 0,
          "empty range must be silent");
    CHECK(sr_stale_check_range(0xFFFFFFFCu, 8u, fake_read, NULL, &bad, &exp, &act) == 0,
          "wrapping range must be silent");
    CHECK(sr_stale_check_range(BLOCK_A, 8u, NULL, NULL, &bad, &exp, &act) == 0,
          "NULL reader must be silent");
    CHECK(sr_stale_check_all(NULL, NULL, &bad, &exp, &act) == 0,
          "NULL reader must be silent (all)");

    /* Misaligned and empty records can never match a fetch: ignored. */
    before = sr_stale_entry_count();
    sr_stale_register_word(BLOCK_A + 1u, 0x12345678u);
    sr_stale_register_block(BLOCK_A + 1u, 2u, PAIR_HASH);
    sr_stale_register_block(BLOCK_A, 0u, PAIR_HASH);
    CHECK(sr_stale_entry_count() == before, "bad records must be ignored");
}

#define INV_BLOCK (FAKE_BASE + 0x40u)

/* TD-27 invalidation half, synthetic self-modifying case. The "AOT body" is
 * the translation-time constant; the "interpreter" re-reads the live fake
 * guest bytes. The tier branch between them is driven by the real
 * sr_stale_block_is_stale() dispatch-side query -- production dispatch must
 * apply the same query at the sites named in src/rt/stale_code.h. */
static uint32_t inv_aot_result(void) {
    return WORD_W0;
}

static uint32_t inv_interp_result(void) {
    uint32_t w = 0u;
    CHECK(fake_read(INV_BLOCK, &w, NULL) == 1, "interp must read the live bytes");
    return w;
}

static void test_invalidation_half(void) {
    static const uint32_t inv_words[2] = { WORD_W0, WORD_W1 };
    static const uint32_t inv_remade[2] = { 0xdeadbeefu, WORD_W1 };
    uint32_t bad = 0u;
    uint32_t exp = 0u;
    uint32_t act = 0u;
    uint32_t tiered;

    sr_stale_reset();
    /* Codegen-shaped registration: entry word plus head-run block. */
    sr_stale_register_word(INV_BLOCK, WORD_W0);
    sr_stale_register_block(INV_BLOCK, 2u, sr_stale_fnv1a(inv_words, 2u));
    fake_write(INV_BLOCK, WORD_W0);
    fake_write(INV_BLOCK + 4u, WORD_W1);

    /* Pristine: the check is silent, the query is clean, dispatch takes AOT. */
    CHECK(sr_stale_check_range(INV_BLOCK, 8u, fake_read, NULL, &bad, &exp, &act) == 0,
          "pristine invalidate must be silent");
    CHECK(sr_stale_block_is_stale(INV_BLOCK) == 0, "pristine block must not be stale");
    CHECK(sr_stale_block_is_stale(INV_BLOCK + 4u) == 0, "interior probe must be clean");
    CHECK(sr_stale_block_is_stale(WORD_B) == 0, "unregistered probe must be clean");
    tiered = sr_stale_block_is_stale(INV_BLOCK) ? inv_interp_result() : inv_aot_result();
    CHECK(tiered == WORD_W0, "pristine dispatch must take the AOT value");

    /* Overwrite one guest word, then run the cache op: the check fires, the
     * flag is set, and the interpreted result follows the new bytes while
     * the stale AOT value still names the old ones. */
    fake_write(INV_BLOCK, 0xdeadbeefu);
    CHECK(sr_stale_check_range(INV_BLOCK, 8u, fake_read, NULL, &bad, &exp, &act) == 1,
          "overwritten invalidate must fire");
    CHECK(sr_stale_block_is_stale(INV_BLOCK) == 1, "firing check must set the flag");
    CHECK(sr_stale_block_is_stale(INV_BLOCK + 4u) == 1,
          "interior dispatch must see the stale block");
    tiered = sr_stale_block_is_stale(INV_BLOCK) ? inv_interp_result() : inv_aot_result();
    CHECK(tiered == 0xdeadbeefu, "stale dispatch must follow the new bytes");
    CHECK(tiered != inv_aot_result(), "interpreted result must differ from stale AOT");

    /* A disjoint probe stays clean while this block is stale. */
    CHECK(sr_stale_block_is_stale(WORD_B) == 0, "disjoint probe must stay clean");

    /* Restore the bytes, then run the cache op again: silent, flag cleared,
     * dispatch returns to the AOT path. */
    fake_write(INV_BLOCK, WORD_W0);
    CHECK(sr_stale_check_range(INV_BLOCK, 8u, fake_read, NULL, &bad, &exp, &act) == 0,
          "restored invalidate must be silent");
    CHECK(sr_stale_block_is_stale(INV_BLOCK) == 0, "restored bytes must clear the flag");
    tiered = sr_stale_block_is_stale(INV_BLOCK) ? inv_interp_result() : inv_aot_result();
    CHECK(tiered == WORD_W0, "restored dispatch must return to the AOT value");

    /* check-all marks and clears the same flag. */
    fake_write(INV_BLOCK + 4u, 0x0bad0badu);
    CHECK(sr_stale_check_all(fake_read, NULL, &bad, &exp, &act) == 1,
          "check-all must fire on the overwrite");
    CHECK(sr_stale_block_is_stale(INV_BLOCK) == 1, "check-all must set the flag");
    fake_write(INV_BLOCK + 4u, WORD_W1);
    CHECK(sr_stale_check_all(fake_read, NULL, &bad, &exp, &act) == 0,
          "check-all must be silent once restored");
    CHECK(sr_stale_block_is_stale(INV_BLOCK) == 0, "check-all must clear the flag");

    /* A fresh registration installs a fresh expectation with the flag clear,
     * without needing another check. */
    fake_write(INV_BLOCK, 0xdeadbeefu);
    CHECK(sr_stale_check_range(INV_BLOCK, 8u, fake_read, NULL, &bad, &exp, &act) == 1,
          "overwrite must fire again");
    CHECK(sr_stale_block_is_stale(INV_BLOCK) == 1, "flag must be set again");
    sr_stale_register_word(INV_BLOCK, 0xdeadbeefu);
    sr_stale_register_block(INV_BLOCK, 2u, sr_stale_fnv1a(inv_remade, 2u));
    CHECK(sr_stale_block_is_stale(INV_BLOCK) == 0,
          "re-registration must clear the flag");
}

int main(void) {
    if (!sr_stale_enabled()) {
        test_gate_off_is_silent();
        if (g_failed) {
            fprintf(stderr, "stale_code selftest: FAILED (gate off)\n");
            return 1;
        }
        printf("stale_code selftest: OK (gate off, silent)\n");
        return 0;
    }
    test_fnv_vectors();
    test_detector_matrix();
    test_invalidation_half();
    sr_stale_reset();
    if (g_failed) {
        fprintf(stderr, "stale_code selftest: FAILED\n");
        return 1;
    }
    printf("stale_code selftest: OK\n");
    return 0;
}
