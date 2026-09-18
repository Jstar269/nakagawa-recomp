// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// TD-27 stale translated-code tracker: table, hash, gate, pure checks, plus
// the invalidation half -- per-entry stale flags and the dispatch-side
// sr_stale_block_is_stale() query. See src/rt/stale_code.h for the contract.
// See src/rt/stale_code.h for the contract. Production guest-memory wiring
// lives in src/rt/hle.c (cache-syscall handlers plus sr_stale_note_cache_op);
// this TU stays free of recomp.h so the selftest links it standalone.

#include "stale_code.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t addr;
    uint32_t expected;
    int stale;
} SrStaleWordEntry;

typedef struct {
    uint32_t addr;
    uint32_t nwords;
    uint32_t expected_hash;
    int stale;
} SrStaleBlockEntry;

static SrStaleWordEntry *s_words;
static size_t s_word_count;
static size_t s_word_capacity;

static SrStaleBlockEntry *s_blocks;
static size_t s_block_count;
static size_t s_block_capacity;

void sr_stale_reset(void) {
    free(s_words);
    s_words = NULL;
    s_word_count = 0;
    s_word_capacity = 0;
    free(s_blocks);
    s_blocks = NULL;
    s_block_count = 0;
    s_block_capacity = 0;
}

static int grow_words(void) {
    size_t next = s_word_capacity == 0u ? 16u : s_word_capacity * 2u;
    SrStaleWordEntry *p = realloc(s_words, next * sizeof(*p));
    if (!p) {
        fprintf(stderr, "STALE_CODE_DETECT: out of memory registering a word record\n");
        abort();
    }
    s_words = p;
    s_word_capacity = next;
    return 1;
}

static int grow_blocks(void) {
    size_t next = s_block_capacity == 0u ? 16u : s_block_capacity * 2u;
    SrStaleBlockEntry *p = realloc(s_blocks, next * sizeof(*p));
    if (!p) {
        fprintf(stderr, "STALE_CODE_DETECT: out of memory registering a block record\n");
        abort();
    }
    s_blocks = p;
    s_block_capacity = next;
    return 1;
}

void sr_stale_register_word(uint32_t addr, uint32_t expected_word) {
    size_t i;
    if ((addr & 3u) != 0u) {
        return;
    }
    for (i = 0; i < s_word_count; i++) {
        if (s_words[i].addr == addr) {
            s_words[i].expected = expected_word;
            s_words[i].stale = 0;
            return;
        }
    }
    if (s_word_count == s_word_capacity) {
        grow_words();
    }
    s_words[s_word_count].addr = addr;
    s_words[s_word_count].expected = expected_word;
    s_words[s_word_count].stale = 0;
    s_word_count++;
}

void sr_stale_register_block(uint32_t addr, uint32_t nwords, uint32_t expected_hash) {
    size_t i;
    uint64_t span;
    if ((addr & 3u) != 0u || nwords == 0u) {
        return;
    }
    span = (uint64_t)nwords * 4u;
    if (span > (uint64_t)0xFFFFFFFFu - (uint64_t)addr) {
        return;
    }
    for (i = 0; i < s_block_count; i++) {
        if (s_blocks[i].addr == addr) {
            s_blocks[i].nwords = nwords;
            s_blocks[i].expected_hash = expected_hash;
            s_blocks[i].stale = 0;
            return;
        }
    }
    if (s_block_count == s_block_capacity) {
        grow_blocks();
    }
    s_blocks[s_block_count].addr = addr;
    s_blocks[s_block_count].nwords = nwords;
    s_blocks[s_block_count].expected_hash = expected_hash;
    s_blocks[s_block_count].stale = 0;
    s_block_count++;
}

uint32_t sr_stale_entry_count(void) {
    return (uint32_t)(s_word_count + s_block_count);
}

int sr_stale_enabled(void) {
    /* Cached getenv (the SR_DISPLOG idiom in src/rt/recomp.c): the disabled
     * path below is one predictable branch with no guest-memory access. */
    static int s_enabled = -1;
    if (s_enabled < 0) {
        const char *e = getenv(SR_STALE_DETECT_ENV);
        s_enabled = (e != NULL && e[0] != '\0' && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return s_enabled;
}

uint32_t sr_stale_fnv1a(const uint32_t *words, uint32_t nwords) {
    uint32_t h = 0x811c9dc5u;
    uint32_t i;
    int shift;
    if (!words) {
        return h;
    }
    for (i = 0; i < nwords; i++) {
        for (shift = 0; shift < 32; shift += 8) {
            h ^= (words[i] >> shift) & 0xffu;
            h *= 0x01000193u;
        }
    }
    return h;
}

static void zero_outs(uint32_t *bad_addr_out, uint32_t *expected_out, uint32_t *actual_out) {
    if (bad_addr_out) {
        *bad_addr_out = 0u;
    }
    if (expected_out) {
        *expected_out = 0u;
    }
    if (actual_out) {
        *actual_out = 0u;
    }
}

/* First/last comparable word addresses for [addr, addr+size). Returns 0 when
 * the range touches no word (size 0 or u32 wrap); malformed ranges are
 * silent, never stale -- argument validation belongs to the HLE caller. */
static int range_words(uint32_t addr, uint32_t size, uint32_t *first_out, uint32_t *last_out) {
    uint32_t end;
    if (size == 0u || addr > 0xFFFFFFFFu - (size - 1u)) {
        return 0;
    }
    end = addr + size - 1u;
    *first_out = addr & ~3u;
    *last_out = end & ~3u;
    return 1;
}

/* Recompute one block record's FNV-1a and update its flag. Returns 1 with
 * *hash_out set on a fully read mismatch, -1 on a fully read match, and 0
 * when any word is unreadable (the flag is left untouched: partial evidence
 * never fires and never clears). Overlap with the caller's window is the
 * caller's business. */
static int mark_block(SrStaleBlockEntry *entry, SrStaleReadFn read, void *ctx,
                      uint32_t *hash_out) {
    uint32_t h = 0x811c9dc5u;
    uint32_t i;
    int shift;
    for (i = 0; i < entry->nwords; i++) {
        uint32_t waddr = entry->addr + i * 4u;
        uint32_t w = 0u;
        if (!read(waddr, &w, ctx)) {
            return 0;
        }
        for (shift = 0; shift < 32; shift += 8) {
            h ^= (w >> shift) & 0xffu;
            h *= 0x01000193u;
        }
    }
    if (h != entry->expected_hash) {
        entry->stale = 1;
        if (hash_out) {
            *hash_out = h;
        }
        return 1;
    }
    entry->stale = 0;
    return -1;
}

/* First-firing report shared by both checks. Words are examined before
 * blocks and records in registration order, so the saved detail always names
 * the earliest mismatch in that order even though every examined record's
 * flag is updated. Prints exactly one STALE_CODE_DETECT line. */
typedef struct {
    int fired;
    int is_block;
    uint32_t addr;
    uint32_t expected;
    uint32_t actual;
    uint32_t nwords;
} SrStaleFire;

static void fire_note_word(SrStaleFire *fire, uint32_t addr, uint32_t expected, uint32_t actual) {
    if (!fire->fired) {
        fire->fired = 1;
        fire->is_block = 0;
        fire->addr = addr;
        fire->expected = expected;
        fire->actual = actual;
    }
}

static void fire_note_block(SrStaleFire *fire, uint32_t addr, uint32_t nwords,
                            uint32_t expected_hash, uint32_t actual_hash) {
    if (!fire->fired) {
        fire->fired = 1;
        fire->is_block = 1;
        fire->addr = addr;
        fire->nwords = nwords;
        fire->expected = expected_hash;
        fire->actual = actual_hash;
    }
}

static int fire_report(const SrStaleFire *fire,
                       uint32_t *bad_addr_out, uint32_t *expected_out, uint32_t *actual_out) {
    if (!fire->fired) {
        zero_outs(bad_addr_out, expected_out, actual_out);
        return 0;
    }
    if (fire->is_block) {
        fprintf(stderr,
                "STALE_CODE_DETECT block=0x%08x nwords=%u expected_hash=0x%08x actual_hash=0x%08x\n",
                fire->addr, fire->nwords, fire->expected, fire->actual);
    } else {
        fprintf(stderr,
                "STALE_CODE_DETECT block=0x%08x off=0x%08x expected=0x%08x actual=0x%08x\n",
                fire->addr, fire->addr, fire->expected, fire->actual);
    }
    if (bad_addr_out) {
        *bad_addr_out = fire->addr;
    }
    if (expected_out) {
        *expected_out = fire->expected;
    }
    if (actual_out) {
        *actual_out = fire->actual;
    }
    return 1;
}

int sr_stale_check_range(uint32_t addr, uint32_t size, SrStaleReadFn read, void *ctx,
                         uint32_t *bad_addr_out, uint32_t *expected_out, uint32_t *actual_out) {
    uint32_t first = 0u;
    uint32_t last = 0u;
    size_t i;
    SrStaleFire fire;
    memset(&fire, 0, sizeof fire);
    if (!sr_stale_enabled()) {
        zero_outs(bad_addr_out, expected_out, actual_out);
        return 0;
    }
    if (!read || !range_words(addr, size, &first, &last)) {
        zero_outs(bad_addr_out, expected_out, actual_out);
        return 0;
    }
    for (i = 0; i < s_word_count; i++) {
        uint32_t actual = 0u;
        SrStaleWordEntry *entry = &s_words[i];
        if (entry->addr < first || entry->addr > last) {
            continue;
        }
        if (!read(entry->addr, &actual, ctx)) {
            continue;
        }
        if (actual != entry->expected) {
            entry->stale = 1;
            fire_note_word(&fire, entry->addr, entry->expected, actual);
            continue;
        }
        entry->stale = 0;
    }
    for (i = 0; i < s_block_count; i++) {
        SrStaleBlockEntry *entry = &s_blocks[i];
        uint32_t block_first = entry->addr;
        uint32_t block_last = entry->addr + entry->nwords * 4u - 4u;
        uint32_t h = 0u;
        if (block_last < first || block_first > last) {
            continue;
        }
        if (mark_block(entry, read, ctx, &h) == 1) {
            fire_note_block(&fire, entry->addr, entry->nwords, entry->expected_hash, h);
        }
    }
    return fire_report(&fire, bad_addr_out, expected_out, actual_out);
}

int sr_stale_check_all(SrStaleReadFn read, void *ctx,
                       uint32_t *bad_addr_out, uint32_t *expected_out, uint32_t *actual_out) {
    size_t i;
    SrStaleFire fire;
    memset(&fire, 0, sizeof fire);
    if (!sr_stale_enabled()) {
        zero_outs(bad_addr_out, expected_out, actual_out);
        return 0;
    }
    if (!read) {
        zero_outs(bad_addr_out, expected_out, actual_out);
        return 0;
    }
    for (i = 0; i < s_word_count; i++) {
        uint32_t actual = 0u;
        SrStaleWordEntry *entry = &s_words[i];
        if (!read(entry->addr, &actual, ctx)) {
            continue;
        }
        if (actual != entry->expected) {
            entry->stale = 1;
            fire_note_word(&fire, entry->addr, entry->expected, actual);
            continue;
        }
        entry->stale = 0;
    }
    for (i = 0; i < s_block_count; i++) {
        SrStaleBlockEntry *entry = &s_blocks[i];
        uint32_t h = 0u;
        if (mark_block(entry, read, ctx, &h) == 1) {
            fire_note_block(&fire, entry->addr, entry->nwords, entry->expected_hash, h);
        }
    }
    return fire_report(&fire, bad_addr_out, expected_out, actual_out);
}

int sr_stale_block_is_stale(uint32_t addr) {
    /* Cached getenv (the SR_DISPLOG idiom): the disabled path is one
     * predictable branch before any table touch, so a future dispatch hook
     * stays off the hot path when the gate is unset. */
    size_t i;
    size_t j;
    if (!sr_stale_enabled()) {
        return 0;
    }
    if ((addr & 3u) != 0u) {
        return 0;
    }
    for (i = 0; i < s_word_count; i++) {
        if (s_words[i].addr == addr && s_words[i].stale) {
            return 1;
        }
    }
    for (i = 0; i < s_block_count; i++) {
        uint64_t span = (uint64_t)s_blocks[i].nwords * 4u;
        if (addr < s_blocks[i].addr || (uint64_t)(addr - s_blocks[i].addr) >= span) {
            continue;
        }
        if (s_blocks[i].stale) {
            return 1;
        }
        if (addr == s_blocks[i].addr) {
            /* Dispatch to a block head whose own hash still matches but an
             * interior word record is stale (the block read was skipped as
             * unreadable while the word stayed comparable): the AOT body
             * would still execute the stale word. Registration rejects
             * wrapping spans, so addr + span cannot wrap here. */
            uint32_t end = s_blocks[i].addr + (uint32_t)span;
            for (j = 0; j < s_word_count; j++) {
                if (s_words[j].stale && s_words[j].addr >= addr && s_words[j].addr < end) {
                    return 1;
                }
            }
        }
    }
    return 0;
}
