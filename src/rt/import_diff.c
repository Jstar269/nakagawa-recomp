// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Differential check of an HLE handler against the game's own disc-shipped module (#2148).
// The contract is in import_diff.h; docs/HLE_AND_WORKAROUND_INVENTORY.md lists the libraries that
// exist in both forms.

#include "recomp.h"
#include "import_diff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The pair table. A row is added only after the handler has been read for host-side state: a
 * handler that keeps any (an object table, a file position, a decoder, the scheduler) carries
 * IMPORT_DIFF_PAIR_HOST_STATE and is reported NOT_COMPARED, because running it beside the export
 * would change what later calls answer. No shipped handler is cleared for comparison yet, so
 * every row is HOST_STATE; a NID with no row is NOT_COMPARED too. The libraries come from the
 * both-forms inventory in docs/HLE_AND_WORKAROUND_INVENTORY.md. */
#define FONT_HOST_STATE "hle_font.c keeps library and font handles on the host, so a handle " \
                        "the disc's libfont made names nothing there"
static const ImportDiffPair k_import_diff_pairs[] = {
    /* sceLibFont (libfont.prx, which discs such as UCUS98701 ship): every call either creates
     * a host-side library or font object or reads one, so none can run beside the export. */
    {"sceLibFont", 0x67F17ED7u, IMPORT_DIFF_PAIR_HOST_STATE, FONT_HOST_STATE}, /* sceFontNewLib */
    {"sceLibFont", 0x574B6FBCu, IMPORT_DIFF_PAIR_HOST_STATE, FONT_HOST_STATE}, /* sceFontDoneLib */
    {"sceLibFont", 0xA834319Du, IMPORT_DIFF_PAIR_HOST_STATE, FONT_HOST_STATE}, /* sceFontOpen */
    {"sceLibFont", 0x3AEA8CB6u, IMPORT_DIFF_PAIR_HOST_STATE, FONT_HOST_STATE}, /* sceFontClose */
    {"sceLibFont", 0x0DA7535Eu, IMPORT_DIFF_PAIR_HOST_STATE, FONT_HOST_STATE}, /* GetFontInfo */
    {"sceLibFont", 0xDCC80C2Fu, IMPORT_DIFF_PAIR_HOST_STATE, FONT_HOST_STATE}, /* GetCharInfo */
    {"sceLibFont", 0x980F4895u, IMPORT_DIFF_PAIR_HOST_STATE, FONT_HOST_STATE}, /* GetCharGlyphImage */
    {"sceLibFont", 0x099EF33Cu, IMPORT_DIFF_PAIR_HOST_STATE, FONT_HOST_STATE}, /* FindOptimumFont */
};

static const ImportDiffPair *s_pairs = k_import_diff_pairs;
static unsigned s_npairs = (unsigned)(sizeof(k_import_diff_pairs) / sizeof(k_import_diff_pairs[0]));

const ImportDiffPair *import_diff_find_pair(uint32_t nid) {
    for (unsigned i = 0; i < s_npairs; i++)
        if (s_pairs[i].nid == nid) return &s_pairs[i];
    return NULL;
}

/* ---- switch and in-flight state ---- */

static int s_enabled = -1;          /* -1 until SR_HLE_DIFF is read */
static int s_in_flight;
static uint32_t s_nested_calls;
static uint32_t s_count[3];

static int import_diff_enabled(void) {
    if (s_enabled < 0) {
        const char *v = getenv("SR_HLE_DIFF");
        s_enabled = v && v[0] == '1' && v[1] == '\0';
        if (s_enabled)
            fprintf(stderr, "HLE_DIFF: on (SR_HLE_DIFF=1): paired disc-module exports also run "
                            "their HLE handler on a copy of guest memory; the export's result is "
                            "kept\n");
    }
    return s_enabled;
}

int import_diff_active(void) { return !s_in_flight && import_diff_enabled(); }

void import_diff_note_syscall(void) {
    if (s_in_flight) s_nested_calls++;
}

void import_diff_counts(uint32_t *match, uint32_t *differ, uint32_t *not_compared) {
    if (match) *match = s_count[IMPORT_DIFF_MATCH];
    if (differ) *differ = s_count[IMPORT_DIFF_DIFFER];
    if (not_compared) *not_compared = s_count[IMPORT_DIFF_NOT_COMPARED];
}

/* ---- covered regions and their snapshots ---- */

#define IMPORT_DIFF_MAX_REGIONS 8u
static ImportDiffRegion s_regions[IMPORT_DIFF_MAX_REGIONS];
static unsigned s_nregions;
static int s_regions_custom;
static uint8_t *s_before[IMPORT_DIFF_MAX_REGIONS];   /* the state both runs start from */
static uint8_t *s_after_hle[IMPORT_DIFF_MAX_REGIONS]; /* what the HLE handler left */
static uint32_t s_snap_size[IMPORT_DIFF_MAX_REGIONS];

/* Guest RAM (kernel and user, 0x08000000 to the end of the 64 MiB arena), VRAM and the
 * scratchpad, as recomp.c lays them out around g_mem. */
static void default_regions(void) {
    s_nregions = 0;
    if (!g_mem) return;
    s_regions[s_nregions++] = (ImportDiffRegion){0x08000000u, 0x04000000u, g_mem};
    s_regions[s_nregions++] =
        (ImportDiffRegion){0x04000000u, 0x00200000u, g_mem - (SR_RAM_BASE - 0x04000000u)};
    s_regions[s_nregions++] =
        (ImportDiffRegion){0x00010000u, 0x00004000u, g_mem - (SR_RAM_BASE - 0x00010000u)};
}

static int snapshots_ready(void) {
    if (!s_regions_custom && s_nregions == 0u) default_regions();
    if (s_nregions == 0u) return 0;
    for (unsigned i = 0; i < s_nregions; i++) {
        if (s_snap_size[i] == s_regions[i].size && s_before[i] && s_after_hle[i]) continue;
        free(s_before[i]);
        free(s_after_hle[i]);
        s_before[i] = (uint8_t *)malloc(s_regions[i].size);
        s_after_hle[i] = (uint8_t *)malloc(s_regions[i].size);
        s_snap_size[i] = s_regions[i].size;
        if (!s_before[i] || !s_after_hle[i]) {
            free(s_before[i]);
            free(s_after_hle[i]);
            s_before[i] = s_after_hle[i] = NULL;
            s_snap_size[i] = 0u;
            return 0;
        }
    }
    return 1;
}

/* The first differing byte of [lo, hi) of region i between the live bytes and the HLE run's,
 * and the number of differing bytes. */
static void compare_span(unsigned i, uint32_t lo, uint32_t hi, ImportDiffResult *r) {
    const ImportDiffRegion *g = &s_regions[i];
    for (uint32_t a = lo; a < hi; a++) {
        uint32_t off = a - g->guest_base;
        uint8_t live = g->host[off], hle = s_after_hle[i][off];
        if (live == hle) continue;
        if (!r->mem_differs || a < r->first_addr) {
            r->first_addr = a;
            r->guest_byte = live;
            r->hle_byte = hle;
        }
        r->mem_differs = 1;
        r->differing_bytes++;
    }
}

static void compare_regions(uint32_t dead_lo, uint32_t dead_hi, ImportDiffResult *r) {
    for (unsigned i = 0; i < s_nregions; i++) {
        const ImportDiffRegion *g = &s_regions[i];
        uint32_t lo = g->guest_base, hi = g->guest_base + g->size;
        if (memcmp(g->host, s_after_hle[i], g->size) == 0) continue;
        if (dead_lo < dead_hi && dead_lo < hi && dead_hi > lo) {
            if (dead_lo > lo) compare_span(i, lo, dead_lo, r);
            if (dead_hi < hi) compare_span(i, dead_hi, hi, r);
        } else {
            compare_span(i, lo, hi, r);
        }
    }
}

static uint32_t f0_bits(const CpuState *s) {
    uint32_t bits;
    memcpy(&bits, &s->f[0], sizeof(bits));
    return bits;
}

/* ---- the check ---- */

static ImportDiffVerdict not_compared(CpuState *s, ImportDiffGuestRun run_guest, void *ctx,
                                   const char *reason, ImportDiffResult *r) {
    if (run_guest) run_guest(s, ctx);
    r->guest_v0 = s->r[2];
    r->guest_v1 = s->r[3];
    r->guest_f0 = f0_bits(s);
    r->verdict = IMPORT_DIFF_NOT_COMPARED;
    r->reason = reason;
    s_count[IMPORT_DIFF_NOT_COMPARED]++;
    return r->verdict;
}

ImportDiffVerdict import_diff_call(CpuState *s, uint32_t nid, const char *name, ImportDiffHleFn hle,
                             uint32_t hle_refusal, ImportDiffGuestRun run_guest, void *ctx,
                             uint32_t dead_lo, uint32_t dead_hi, ImportDiffResult *out) {
    ImportDiffResult local;
    ImportDiffResult *r = out ? out : &local;
    memset(r, 0, sizeof(*r));
    r->nid = nid;
    r->name = name;
    for (int i = 0; i < 8; i++) r->args[i] = s->r[4 + i];

    const ImportDiffPair *pair = import_diff_find_pair(nid);
    if (pair) r->library = pair->library;
    if (!pair) return not_compared(s, run_guest, ctx, "no pair-table row", r);
    if ((pair->flags & IMPORT_DIFF_PAIR_HOST_STATE) || !(pair->flags & IMPORT_DIFF_PAIR_COMPARE))
        return not_compared(s, run_guest, ctx, "the HLE handler keeps host state", r);
    if (!hle && hle_refusal) return not_compared(s, run_guest, ctx,
                                                 "the HLE registration is a controlled refusal", r);
    if (!hle) return not_compared(s, run_guest, ctx, "no HLE handler for the NID", r);
    if (s_in_flight) return not_compared(s, run_guest, ctx, "a comparison is already in flight", r);
    if (!snapshots_ready())
        return not_compared(s, run_guest, ctx, "no memory for the guest-memory copies", r);

    /* 1. Snapshot, then run the HLE handler on the live bytes; keep what it left. */
    CpuState before = *s;
    for (unsigned i = 0; i < s_nregions; i++)
        memcpy(s_before[i], s_regions[i].host, s_regions[i].size);
    s_in_flight = 1;
    s_nested_calls = 0u;
    uint32_t hle_v0 = hle(s);
    uint32_t hle_nested = s_nested_calls;
    r->hle_v0 = hle_v0;
    r->hle_v1 = s->r[3];
    r->hle_f0 = f0_bits(s);
    for (unsigned i = 0; i < s_nregions; i++)
        memcpy(s_after_hle[i], s_regions[i].host, s_regions[i].size);

    /* 2. Put the starting state back and run the guest export: this is the run the game keeps. */
    *s = before;
    for (unsigned i = 0; i < s_nregions; i++)
        memcpy(s_regions[i].host, s_before[i], s_regions[i].size);
    s_nested_calls = 0u;
    run_guest(s, ctx);
    uint32_t guest_nested = s_nested_calls;
    s_in_flight = 0;
    r->guest_v0 = s->r[2];
    r->guest_v1 = s->r[3];
    r->guest_f0 = f0_bits(s);

    if (hle_nested || guest_nested) {
        r->verdict = IMPORT_DIFF_NOT_COMPARED;
        r->reason = hle_nested ? "the HLE handler made a nested import call"
                               : "the guest export made a nested import call";
        s_count[IMPORT_DIFF_NOT_COMPARED]++;
        return r->verdict;
    }

    /* 3. Compare. */
    r->ret_differs = r->guest_v0 != r->hle_v0 ||
                     ((pair->flags & IMPORT_DIFF_RET_V1) && r->guest_v1 != r->hle_v1) ||
                     ((pair->flags & IMPORT_DIFF_RET_F0) && r->guest_f0 != r->hle_f0);
    compare_regions(dead_lo, dead_hi, r);
    r->verdict = (r->ret_differs || r->mem_differs) ? IMPORT_DIFF_DIFFER : IMPORT_DIFF_MATCH;
    s_count[r->verdict]++;
    return r->verdict;
}

/* ---- report ---- */

void import_diff_format(const ImportDiffResult *r, char *buf, size_t size) {
    static const char *const names[] = {"MATCH", "DIFFER", "NOT_COMPARED"};
    if (!buf || size == 0u) return;
    int n = snprintf(buf, size,
                     "HLE_DIFF %s nid=0x%08x name=%s lib=%s args=%08x,%08x,%08x,%08x,%08x,%08x,"
                     "%08x,%08x",
                     names[r->verdict], r->nid, r->name ? r->name : "?",
                     r->library ? r->library : "-", r->args[0], r->args[1], r->args[2], r->args[3],
                     r->args[4], r->args[5], r->args[6], r->args[7]);
    if (n < 0 || (size_t)n >= size) return;
    size_t at = (size_t)n;
    if (r->verdict == IMPORT_DIFF_NOT_COMPARED) {
        snprintf(buf + at, size - at, " v0=%08x reason=%s", r->guest_v0,
                 r->reason ? r->reason : "?");
        return;
    }
    if (!r->ret_differs) {
        n = snprintf(buf + at, size - at, " v0=%08x", r->guest_v0);
    } else {
        n = snprintf(buf + at, size - at, " ret guest=%08x:%08x:%08x hle=%08x:%08x:%08x",
                     r->guest_v0, r->guest_v1, r->guest_f0, r->hle_v0, r->hle_v1, r->hle_f0);
    }
    if (n < 0 || (size_t)n >= size - at) return;
    at += (size_t)n;
    if (r->mem_differs)
        snprintf(buf + at, size - at, " first=0x%08x guest=0x%02x hle=0x%02x bytes=%u",
                 r->first_addr, r->guest_byte, r->hle_byte, r->differing_bytes);
}

#define IMPORT_DIFF_SEEN_MAX 256u
#define IMPORT_DIFF_DIFFER_LINES 32u
static struct {
    uint32_t nid;
    uint8_t seen_match, seen_not_compared;
    uint32_t differ_lines;
} s_seen[IMPORT_DIFF_SEEN_MAX];
static unsigned s_nseen;

void import_diff_report(const ImportDiffResult *r) {
    unsigned k = 0;
    while (k < s_nseen && s_seen[k].nid != r->nid) k++;
    if (k == s_nseen) {
        if (s_nseen == IMPORT_DIFF_SEEN_MAX) k = IMPORT_DIFF_SEEN_MAX; /* past the table: print all */
        else s_seen[s_nseen++].nid = r->nid;
    }
    int print = 1;
    if (k < IMPORT_DIFF_SEEN_MAX) {
        if (r->verdict == IMPORT_DIFF_MATCH) {
            print = !s_seen[k].seen_match;
            s_seen[k].seen_match = 1;
        } else if (r->verdict == IMPORT_DIFF_NOT_COMPARED) {
            print = !s_seen[k].seen_not_compared;
            s_seen[k].seen_not_compared = 1;
        } else {
            print = s_seen[k].differ_lines < IMPORT_DIFF_DIFFER_LINES;
            s_seen[k].differ_lines++;
        }
    }
    if (!print) return;
    char line[512];
    import_diff_format(r, line, sizeof(line));
    fprintf(stderr, "%s\n", line);
}

#ifdef SR_HLE_THREAD_SELFTEST
void import_diff_test_set_enabled(int on) { s_enabled = on ? 1 : 0; }

void import_diff_test_set_pairs(const ImportDiffPair *pairs, unsigned count) {
    if (pairs) {
        s_pairs = pairs;
        s_npairs = count;
    } else {
        s_pairs = k_import_diff_pairs;
        s_npairs = (unsigned)(sizeof(k_import_diff_pairs) / sizeof(k_import_diff_pairs[0]));
    }
}

void import_diff_test_set_regions(const ImportDiffRegion *regions, unsigned count) {
    if (regions && count <= IMPORT_DIFF_MAX_REGIONS) {
        memcpy(s_regions, regions, count * sizeof(regions[0]));
        s_nregions = count;
        s_regions_custom = 1;
    } else {
        s_nregions = 0;
        s_regions_custom = 0;
    }
}

void import_diff_test_reset(void) {
    memset(s_count, 0, sizeof(s_count));
    memset(s_seen, 0, sizeof(s_seen));
    s_nseen = 0;
    s_in_flight = 0;
    s_nested_calls = 0u;
}
#endif
