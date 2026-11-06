/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * motion_match_selftest.c - the draw matcher of render-state motion (motion_match.h, #2081) on
 * synthetic PRIM lists: anchors and their ordered run, reorder, spawn and despawn, a rebuild at a
 * new address, ties, a spawn inside a run of equal signatures, inline keys that never anchor,
 * teleports and both distance bounds exactly, a vertex-count change, the camera in the GE's own
 * matrix order, every frame cut, the camera history and its median, non-finite cameras, no
 * previous frame, invalid arguments, the LCS and large-leaf paths, and a randomized pass over the
 * invariants and determinism.
 *
 * Built from motion_match.c and this file alone, with nothing else of the runtime.
 */
#include "motion_match.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;

#define CHECK(cond, ...) do { \
        if (!(cond)) { s_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                       fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
    } while (0)

#define MAXN 256u
#define N_ (-1)
#define AN MOTION_PAIR_ANCHOR
#define FI MOTION_PAIR_FILL
#define RE MOTION_PAIR_REORDER
#define TP MOTION_PAIR_TELEPORT
#define NO MOTION_PAIR_NONE

/* A transform-mode triangle list of six vertices; `sig` picks its texture, so equal sig means
 * equal signature. Its box is 8 px square at (x, y). */
static MotionPrim prim(uint32_t sig, uint32_t vaddr, float x, float y) {
    MotionPrim p;
    memset(&p, 0, sizeof(p));
    p.prim_type = 3;
    p.vtype = 0x011Fu;
    p.count = 6;
    p.tex_addr = 0x04000000u + sig * 0x1000u;
    p.vaddr = vaddr;
    p.bbox[0] = x;
    p.bbox[1] = y;
    p.bbox[2] = x + 8.0f;
    p.bbox[3] = y + 8.0f;
    return p;
}

static MotionPrim moved(MotionPrim p, float dx, float dy) {
    p.bbox[0] += dx;
    p.bbox[2] += dx;
    p.bbox[1] += dy;
    p.bbox[3] += dy;
    return p;
}

static int32_t g_pa[MAXN];
static uint8_t g_why[MAXN];
static MotionMatchResult g_r;

static MotionMatchFrame frame(const MotionPrim *p, uint32_t n) {
    MotionMatchFrame f;
    memset(&f, 0, sizeof(f));
    f.prims = p;
    f.nprims = n;
    return f;
}

/* No camera in either frame: the camera rules stay off. */
static int match(const MotionPrim *a, uint32_t na, const MotionPrim *b, uint32_t nb) {
    MotionMatchFrame fa = frame(a, na), fb = frame(b, nb);
    MotionMatchParams p;
    motion_match_default_params(&p);
    return motion_match(&fa, &fb, &p, NULL, g_pa, g_why, &g_r);
}

static void expect(const char *cell, uint32_t nb, const int32_t *pa, const uint8_t *why) {
    for (uint32_t j = 0; j < nb; j++) {
        CHECK(g_pa[j] == pa[j], "%s: B[%u] pairs with A[%d], expected A[%d]", cell, j, g_pa[j], pa[j]);
        CHECK(g_why[j] == why[j], "%s: B[%u] reason %u, expected %u", cell, j, g_why[j], why[j]);
    }
}

/* ---- pairing ------------------------------------------------------------------------------ */

static void cell_identical(void) {
    MotionPrim a[5];
    for (uint32_t k = 0; k < 5; k++) a[k] = prim(k % 2u, 0x08800000u + k * 0x100u, 40.0f * (float)k, 20.0f);
    CHECK(match(a, 5, a, 5) == 0, "identical: returns 0");
    static const int32_t pa[5] = { 0, 1, 2, 3, 4 };
    static const uint8_t why[5] = { AN, AN, AN, AN, AN };
    expect("identical", 5, pa, why);
    CHECK(g_r.flags == 0 && g_r.anchors == 5 && g_r.fills == 0 && g_r.reorders == 0 && g_r.teleports == 0 &&
          g_r.unmatched == 0, "identical: stats (flags %#x, anchors %u)", g_r.flags, g_r.anchors);
    CHECK(g_r.xform_verts == 30 && g_r.xform_verts_matched == 30, "identical: vertex mass 30 of 30");
}

/* One draw moved in the list: the other four stay the ordered run, it pairs by reorder. */
static void cell_reorder(void) {
    MotionPrim a[5], b[5];
    for (uint32_t k = 0; k < 5; k++) a[k] = prim(7, 0x08800000u + k * 0x100u, 30.0f * (float)k, 0.0f);
    b[0] = a[0]; b[1] = a[3]; b[2] = a[1]; b[3] = a[2]; b[4] = a[4];
    CHECK(match(a, 5, b, 5) == 0, "reorder: returns 0");
    static const int32_t pa[5] = { 0, 3, 1, 2, 4 };
    static const uint8_t why[5] = { AN, RE, AN, AN, AN };
    expect("reorder", 5, pa, why);
    CHECK(g_r.anchors == 4 && g_r.reorders == 1, "reorder: 4 anchors, 1 reorder (%u, %u)", g_r.anchors, g_r.reorders);
}

/* The ordered run is the longest one, not a prefix: B = k0 k2 k3 k1 keeps k0 k2 k3 (A indices
 * 0, 2, 3, a run that is not contiguous in the candidates) and reorders k1. */
static void cell_longest_run(void) {
    MotionPrim a[4], b[4];
    for (uint32_t k = 0; k < 4; k++) a[k] = prim(1, 0x08900000u + k * 0x40u, 0.0f, 0.0f);
    b[0] = a[0]; b[1] = a[2]; b[2] = a[3]; b[3] = a[1];
    CHECK(match(a, 4, b, 4) == 0, "longest run: returns 0");
    static const int32_t pa[4] = { 0, 2, 3, 1 };
    static const uint8_t why[4] = { AN, AN, AN, RE };
    expect("longest run", 4, pa, why);
}

/* A draw that leaves and a new one with another signature: neither takes the other's place. */
static void cell_spawn_despawn(void) {
    MotionPrim a[4], b[4];
    for (uint32_t k = 0; k < 4; k++) a[k] = prim(k, 0x08800000u + k * 0x100u, 50.0f * (float)k, 0.0f);
    b[0] = a[0]; b[1] = prim(9, 0x08A00000u, 50.0f, 0.0f); b[2] = a[2]; b[3] = a[3];
    CHECK(match(a, 4, b, 4) == 0, "spawn: returns 0");
    static const int32_t pa[4] = { 0, N_, 2, 3 };
    static const uint8_t why[4] = { AN, NO, AN, AN };
    expect("spawn and despawn", 4, pa, why);
    CHECK(g_r.unmatched == 1 && g_r.anchors == 3, "spawn: 1 unmatched, 3 anchors");
}

/* The same object rebuilt into another vertex buffer: its key changes, its signature does not. */
static void cell_rebuild(void) {
    MotionPrim a[3] = { prim(0, 0x08800000u, 0, 0), prim(1, 0x09000000u, 100, 100), prim(2, 0x08800100u, 200, 0) };
    MotionPrim b[3] = { a[0], prim(1, 0x09100000u, 104, 101), a[2] };
    CHECK(match(a, 3, b, 3) == 0, "rebuild: returns 0");
    static const int32_t pa[3] = { 0, 1, 2 };
    static const uint8_t why[3] = { AN, FI, AN };
    expect("rebuild at a new address", 3, pa, why);
    CHECK(g_r.fills == 1, "rebuild: one fill");

    /* the same rebuild, moved further than d_max: found, then dropped */
    b[1] = prim(1, 0x09100000u, 200, 100);
    CHECK(match(a, 3, b, 3) == 0, "rebuild far: returns 0");
    static const int32_t pa2[3] = { 0, N_, 2 };
    static const uint8_t why2[3] = { AN, TP, AN };
    expect("rebuild moved too far", 3, pa2, why2);
    CHECK(g_r.teleports == 1 && g_r.unmatched == 0, "rebuild far: a teleport, not an unmatched");
}

/* One buffer drawn three times (equal keys, so no anchor): pairs in order. */
static void cell_ties(void) {
    MotionPrim a[5], b[5];
    a[0] = prim(0, 0x08800000u, 0, 0);
    for (uint32_t k = 1; k < 4; k++) a[k] = prim(5, 0x09000000u, 90.0f * (float)k, 50.0f);
    a[4] = prim(2, 0x08800100u, 400, 0);
    for (uint32_t k = 0; k < 5; k++) b[k] = moved(a[k], 3.0f, -2.0f);
    CHECK(match(a, 5, b, 5) == 0, "ties: returns 0");
    static const int32_t pa[5] = { 0, 1, 2, 3, 4 };
    static const uint8_t why[5] = { AN, FI, FI, FI, AN };
    expect("ties in order", 5, pa, why);
}

/* A run of equal signatures (particles, 100 px apart) with one spawned at its front: each old
 * particle keeps its own partner instead of every pair shifting by one. */
static void cell_spawn_in_run(void) {
    MotionPrim a[7], b[8];
    a[0] = prim(0, 0x08800000u, 0, 0);
    for (uint32_t k = 0; k < 5; k++) a[1 + k] = prim(6, 0x09200000u + k * 0x60u, 100.0f * (float)k, 150.0f);
    a[6] = prim(2, 0x08800100u, 0, 250);
    b[0] = a[0];
    b[1] = prim(6, 0x09300000u, 0.0f, 0.0f);        /* the new particle, far from every old one */
    for (uint32_t k = 0; k < 5; k++) b[2 + k] = moved(prim(6, 0x09300060u + k * 0x60u, 100.0f * (float)k, 150.0f), 2, 0);
    b[7] = a[6];
    CHECK(match(a, 7, b, 8) == 0, "spawn in run: returns 0");
    static const int32_t pa[8] = { 0, N_, 1, 2, 3, 4, 5, 6 };
    static const uint8_t why[8] = { AN, NO, FI, FI, FI, FI, FI, AN };
    expect("spawn in a run", 8, pa, why);
    CHECK(g_r.teleports == 0 && g_r.unmatched == 1, "spawn in run: no teleport, one unmatched");
}

/* Despawn and spawn inside a run of ten: the middle goes to the exact LCS leaf. */
static void cell_lcs_leaf(void) {
    MotionPrim a[12], b[12];
    uint32_t nb = 0;
    a[0] = prim(0, 0x08800000u, 0, 0);
    for (uint32_t k = 0; k < 10; k++) a[1 + k] = prim(6, 0x09200000u + k * 0x60u, 100.0f * (float)(k % 5u), 100.0f + 100.0f * (float)(k / 5u));
    a[11] = prim(2, 0x08800100u, 0, 400);
    int32_t pa[12];
    uint8_t why[12];
    b[nb] = a[0]; pa[nb] = 0; why[nb++] = AN;
    for (uint32_t k = 0; k < 10; k++) {
        if (k == 2) continue;   /* despawned */
        if (k == 7) { b[nb] = prim(6, 0x09400000u, 1000, 1000); pa[nb] = N_; why[nb++] = NO; }
        b[nb] = moved(a[1 + k], 1, 1);
        b[nb].vaddr += 0x10000u;
        pa[nb] = (int32_t)(1 + k);
        why[nb++] = FI;
    }
    b[nb] = a[11]; pa[nb] = 11; why[nb++] = AN;
    CHECK(match(a, 12, b, nb) == 0, "lcs leaf: returns 0");
    expect("LCS leaf", nb, pa, why);
}

/* A small leaf takes the longest common subsequence, not the nearest partner: B[1] at x 115 is
 * nearer A's 120, but pairing it with 100 lets B[2] at 140 pair with 120, two pairs instead of one. */
static void cell_lcs_not_nearest(void) {
    MotionPrim a[5] = { prim(0, 0x08800000u, -900, 0), prim(6, 0x09200000u, 0, 0), prim(6, 0x09200040u, 100, 0),
                        prim(6, 0x09200080u, 120, 0), prim(2, 0x08800100u, 900, 0) };
    MotionPrim b[5] = { a[0], prim(6, 0x09300000u, 115, 0), prim(6, 0x09300040u, 140, 0), prim(6, 0x09300080u, 400, 0),
                        a[4] };
    CHECK(match(a, 5, b, 5) == 0, "lcs not nearest: returns 0");
    static const int32_t pa[5] = { 0, 2, 3, N_, 4 };
    static const uint8_t why[5] = { AN, FI, FI, NO, AN };
    expect("LCS, not nearest", 5, pa, why);
}

/* A key drawn twice in A and once in B is no anchor: B's copy goes to the fill, which pairs it
 * with the plausible one of the two. */
static void cell_key_unique_in_a(void) {
    MotionPrim a[4] = { prim(0, 0x08800000u, -900, 0), prim(6, 0x09200000u, 0, 0), prim(6, 0x09200000u, 200, 0),
                        prim(2, 0x08800100u, 900, 0) };
    MotionPrim b[3] = { a[0], prim(6, 0x09200000u, 201, 0), a[3] };
    CHECK(match(a, 4, b, 3) == 0, "key twice in A: returns 0");
    static const int32_t pa[3] = { 0, 2, 3 };
    static const uint8_t why[3] = { AN, FI, AN };
    expect("key twice in A", 3, pa, why);
}

/* F13: a vertex ring reused at the same offsets puts another object at the same address with the
 * same signature. Marked inline, that key never anchors: the real object, rebuilt elsewhere, pairs
 * by the fill, and the stranger stays unmatched instead of a false anchor that teleports. */
static void cell_inline_never_anchors(void) {
    MotionPrim a[3] = { prim(0, 0x08800000u, -900, 0), prim(6, 0x09200000u, 0, 0), prim(2, 0x08800100u, 900, 0) };
    MotionPrim b[4] = { a[0], prim(6, 0x09200000u, 300, 0), prim(6, 0x09300000u, 2, 0), a[2] };
    a[1].flags = b[1].flags = MOTION_PRIM_INLINE;
    CHECK(match(a, 3, b, 4) == 0, "inline: returns 0");
    static const int32_t pa[4] = { 0, N_, 1, 2 };
    static const uint8_t why[4] = { AN, NO, FI, AN };
    expect("inline never anchors", 4, pa, why);
    /* marked in one frame only is enough */
    b[1].flags = 0;
    CHECK(match(a, 3, b, 4) == 0 && g_pa[2] == 1 && g_why[1] == NO, "inline in A only never anchors");
    a[1].flags = 0;
    b[1].flags = MOTION_PRIM_INLINE;
    CHECK(match(a, 3, b, 4) == 0 && g_pa[2] == 1 && g_why[1] == NO, "inline in B only never anchors");
}

static int match_p(const MotionPrim *a, uint32_t na, const MotionPrim *b, uint32_t nb, const MotionMatchParams *p) {
    MotionMatchFrame fa = frame(a, na), fb = frame(b, nb);
    return motion_match(&fa, &fb, p, NULL, g_pa, g_why, &g_r);
}

/* Both distance bounds are inclusive, tested exactly at the bound (a 30-40-50 step is exact in float). */
static void cell_exact_bounds(void) {
    MotionMatchParams p;
    motion_match_default_params(&p);
    p.d_max = 50.0f;
    MotionPrim a[2] = { prim(0, 0x08800000u, 0, 0), prim(1, 0x09000000u, 100, 100) };
    MotionPrim b[2] = { a[0], moved(a[1], 30, 40) };
    CHECK(match_p(a, 2, b, 2, &p) == 0 && g_pa[1] == 1 && g_why[1] == AN, "d_max: exactly 50 pairs (%d %u)", g_pa[1],
          g_why[1]);
    b[1] = moved(a[1], 30, 40.01f);
    CHECK(match_p(a, 2, b, 2, &p) == 0 && g_pa[1] == N_ && g_why[1] == TP, "d_max: just over 50 is a teleport");

    p.d_leaf = 50.0f;
    p.d_max = 1000.0f;   /* so a leaf miss cannot be a teleport */
    MotionPrim c[4] = { prim(0, 0x08800000u, -900, 0), prim(6, 0x09200000u, 0, 0), prim(6, 0x09200040u, 300, 0),
                        prim(2, 0x08800100u, 900, 0) };
    MotionPrim d[4] = { c[0], moved(c[1], 30, 40), moved(c[2], 1, 0), c[3] };
    d[1].vaddr = 0x09300000u;
    d[2].vaddr = 0x09300040u;
    CHECK(match_p(c, 4, d, 4, &p) == 0 && g_pa[1] == 1 && g_why[1] == FI, "d_leaf: exactly 50 pairs in a leaf (%d %u)",
          g_pa[1], g_why[1]);
    d[1] = moved(c[1], 30, 40.01f);
    d[1].vaddr = 0x09300000u;
    CHECK(match_p(c, 4, d, 4, &p) == 0 && g_pa[1] == N_ && g_why[1] == NO && g_pa[2] == 2 && g_r.teleports == 0,
          "d_leaf: just over 50 does not pair in a leaf (%d %u)", g_pa[1], g_why[1]);
}

/* A gap signature unique in B but repeated in A does not anchor the gap: the leaf pairs it with
 * the plausible member, here the first, instead of whichever the hash order offered. */
static void cell_gap_sig_repeated_in_a(void) {
    MotionPrim a[4] = { prim(0, 0x08800000u, -900, 0), prim(6, 0x09200000u, 0, 0), prim(6, 0x09200040u, 200, 0),
                        prim(2, 0x08800100u, 900, 0) };
    MotionPrim b[3] = { a[0], prim(6, 0x09300000u, 1, 0), a[3] };
    CHECK(match(a, 4, b, 3) == 0, "repeated in A: returns 0");
    static const int32_t pa[3] = { 0, 1, 3 };
    static const uint8_t why[3] = { AN, FI, AN };
    expect("gap signature repeated in A", 3, pa, why);
}

/* LCS ties: A0 and A1 cross B1 and B0. Only one pair can keep the order; the earlier B keeps its
 * partner (B0 with A1), and B1 is unmatched. */
static void cell_lcs_tie(void) {
    MotionPrim a[4] = { prim(0, 0x08800000u, -900, 0), prim(6, 0x09200000u, 0, 0), prim(6, 0x09200040u, 50, 0),
                        prim(2, 0x08800100u, 900, 0) };
    MotionPrim b[4] = { a[0], prim(6, 0x09300000u, 50, 20), prim(6, 0x09300040u, 0, 20), a[3] };
    CHECK(match(a, 4, b, 4) == 0, "lcs tie: returns 0");
    static const int32_t pa[4] = { 0, 2, N_, 3 };
    static const uint8_t why[4] = { AN, FI, NO, AN };
    expect("LCS tie", 4, pa, why);
}

/* The large leaf takes the nearest candidate, not the first: A's 11th particle sits 15 px from the
 * 10th, and B dropped the 10th. */
static void cell_large_leaf_nearest(void) {
    static MotionPrim a[82], b[83];
    static int32_t pa[83];
    static uint8_t why[83];
    uint32_t nb = 0;
    a[0] = prim(0, 0x08800000u, -500, -500);
    for (uint32_t k = 0; k < 80; k++) a[1 + k] = prim(6, 0x09200000u + k * 0x60u, 100.0f * (float)k, 0.0f);
    a[1 + 11] = prim(6, 0x09200000u + 11u * 0x60u, 1015.0f, 0.0f);
    a[81] = prim(2, 0x08800100u, -500, 2000);
    b[nb] = a[0]; pa[nb] = 0; why[nb++] = AN;
    b[nb] = prim(6, 0x09400000u, 5000, 5000); pa[nb] = N_; why[nb++] = NO;   /* keeps the prefix strip off */
    for (uint32_t k = 0; k < 80; k++) {
        if (k == 10) continue;
        b[nb] = a[1 + k];
        b[nb].vaddr += 0x10000u;
        pa[nb] = (int32_t)(1 + k);
        why[nb++] = FI;
    }
    b[nb] = prim(6, 0x09400040u, 6000, 5000); pa[nb] = N_; why[nb++] = NO;   /* and the suffix strip */
    b[nb] = a[81]; pa[nb] = 81; why[nb++] = AN;
    CHECK(match(a, 82, b, nb) == 0, "large leaf nearest: returns 0");
    expect("large leaf, nearest", nb, pa, why);
}

/* The same with a hundred particles: the middle is larger than the LCS bound, so the bounded
 * in-order pass pairs it. */
static void cell_large_leaf(void) {
    static MotionPrim a[102], b[102];
    static int32_t pa[102];
    static uint8_t why[102];
    uint32_t nb = 0;
    a[0] = prim(0, 0x08800000u, -500, -500);
    for (uint32_t k = 0; k < 100; k++) a[1 + k] = prim(6, 0x09200000u + k * 0x60u, 100.0f * (float)(k % 10u), 100.0f * (float)(k / 10u));
    a[101] = prim(2, 0x08800100u, -500, 2000);
    b[nb] = a[0]; pa[nb] = 0; why[nb++] = AN;
    for (uint32_t k = 0; k < 100; k++) {
        if (k == 5) continue;
        if (k == 90) { b[nb] = prim(6, 0x09400000u, 5000, 5000); pa[nb] = N_; why[nb++] = NO; }
        b[nb] = moved(a[1 + k], 1, 0);
        b[nb].vaddr += 0x10000u;
        pa[nb] = (int32_t)(1 + k);
        why[nb++] = FI;
    }
    b[nb] = a[101]; pa[nb] = 101; why[nb++] = AN;
    CHECK(match(a, 102, b, nb) == 0, "large leaf: returns 0");
    expect("large leaf", nb, pa, why);
    CHECK(g_r.fills == 99 && g_r.unmatched == 1, "large leaf: 99 fills (%u), 1 unmatched (%u)", g_r.fills, g_r.unmatched);
}

/* A signature unique to the gap anchors it, and the ties on either side still pair. */
static void cell_nested_gap(void) {
    MotionPrim a[5] = { prim(0, 0x08800000u, 0, 0), prim(5, 0x09000000u, 100, 0), prim(8, 0x09100000u, 200, 0),
                        prim(5, 0x09000000u, 300, 0), prim(2, 0x08800100u, 400, 0) };
    MotionPrim b[5] = { a[0], moved(a[1], 1, 0), moved(a[2], 1, 0), moved(a[3], 1, 0), a[4] };
    b[1].vaddr = b[3].vaddr = 0x09500000u;
    b[2].vaddr = 0x09600000u;
    CHECK(match(a, 5, b, 5) == 0, "nested gap: returns 0");
    static const int32_t pa[5] = { 0, 1, 2, 3, 4 };
    static const uint8_t why[5] = { AN, FI, FI, FI, AN };
    expect("nested gap", 5, pa, why);
}

static void cell_count_change(void) {
    MotionPrim a[3] = { prim(0, 0x08800000u, 0, 0), prim(1, 0x09000000u, 100, 0), prim(2, 0x08800100u, 200, 0) };
    MotionPrim b[3] = { a[0], a[1], a[2] };
    b[1].count = 9;
    CHECK(match(a, 3, b, 3) == 0, "count: returns 0");
    static const int32_t pa[3] = { 0, N_, 2 };
    static const uint8_t why[3] = { AN, NO, AN };
    expect("vertex count change", 3, pa, why);
}

static void cell_teleport(void) {
    MotionPrim a[2] = { prim(0, 0x08800000u, 0, 0), prim(1, 0x09000000u, 100, 0) };
    MotionPrim b[2] = { a[0], moved(a[1], 60, 40) };   /* 72.1 px: just over d_max */
    CHECK(match(a, 2, b, 2) == 0, "teleport: returns 0");
    static const int32_t pa[2] = { 0, N_ };
    static const uint8_t why[2] = { AN, TP };
    expect("teleport", 2, pa, why);
    b[1] = moved(a[1], 60, 30);                         /* 67.1 px: inside */
    CHECK(match(a, 2, b, 2) == 0 && g_pa[1] == 1 && g_why[1] == AN, "just inside d_max pairs");
    b[1] = a[1];
    b[1].bbox[2] = NAN;
    CHECK(match(a, 2, b, 2) == 0 && g_pa[1] == N_ && g_why[1] == TP, "a non-finite box never pairs");
}

/* ---- the frame ---------------------------------------------------------------------------- */

static void cell_no_previous(void) {
    MotionPrim b[3] = { prim(0, 1, 0, 0), prim(1, 2, 0, 0), prim(2, 3, 0, 0) };
    b[2].through = 1;
    memset(g_pa, 0x55, sizeof(g_pa));
    CHECK(match(NULL, 0, b, 3) == 0, "no previous: returns 0");
    CHECK(g_r.flags == MOTION_MATCH_NO_PREVIOUS && g_r.unmatched == 3 && g_r.xform_verts == 12,
          "no previous: flag, 3 unmatched, 12 transform vertices (%#x %u %llu)", g_r.flags, g_r.unmatched,
          (unsigned long long)g_r.xform_verts);
    CHECK(g_pa[0] == N_ && g_pa[1] == N_ && g_pa[2] == N_ && g_why[0] == NO, "no previous: nothing paired");
    CHECK(match(b, 3, NULL, 0) == 0 && g_r.flags == 0 && g_r.unmatched == 0, "an empty B is not an error");
}

static void cell_failed(void) {
    MotionPrim a[2] = { prim(0, 1, 0, 0), prim(1, 2, 0, 0) };
    MotionMatchFrame fa = frame(a, 2), fb = frame(a, 2), bad = frame(NULL, 2);
    MotionMatchParams p;
    motion_match_default_params(&p);
    memset(g_pa, 0x55, sizeof(g_pa));
    memset(g_why, AN, sizeof(g_why));
    CHECK(motion_match(NULL, &fb, &p, NULL, g_pa, g_why, &g_r) == -1, "no A: -1");
    CHECK(g_r.flags == MOTION_MATCH_FAILED && g_r.unmatched == 2 && g_pa[0] == N_ && g_pa[1] == N_ &&
          g_why[0] == NO && g_why[1] == NO, "no A: FAILED, nothing paired");
    CHECK(motion_match(&fa, &bad, &p, NULL, g_pa, g_why, &g_r) == -1 && g_r.flags == MOTION_MATCH_FAILED,
          "B with a count and no PRIMs: FAILED");
    CHECK(motion_match(&bad, &fb, &p, NULL, g_pa, g_why, &g_r) == -1 && g_pa[0] == N_, "A with a count and no PRIMs");
    CHECK(motion_match(&fa, &fb, NULL, NULL, g_pa, g_why, &g_r) == -1 && g_r.flags == MOTION_MATCH_FAILED,
          "no params: FAILED");
    CHECK(motion_match(&fa, &fb, &p, NULL, NULL, g_why, &g_r) == -1 && g_r.flags == MOTION_MATCH_FAILED,
          "no pair_a: FAILED");
    CHECK(motion_match(&fa, &fb, &p, NULL, g_pa, g_why, NULL) == -1, "no result: -1");
    memset(g_why, AN, sizeof(g_why));
    CHECK(motion_match(&fa, &fb, &p, NULL, NULL, g_why, &g_r) == -1 && g_why[0] == NO && g_why[1] == NO,
          "no pair_a: the reasons are still cleared");
    MotionMatchFrame big = frame(a, MOTION_MATCH_MAX_PRIMS + 1u);
    CHECK(motion_match(&big, &fb, &p, NULL, g_pa, g_why, &g_r) == -1 && g_r.flags == MOTION_MATCH_FAILED,
          "more PRIMs than the cap: FAILED");

    /* a caller-owned history out of range, or holding a non-finite step */
    MotionCutHistory h;
    memset(&h, 0, sizeof(h));
    h.next = MOTION_CUT_HISTORY;
    CHECK(motion_match(&fa, &fb, &p, &h, g_pa, g_why, &g_r) == -1 && g_r.flags == MOTION_MATCH_FAILED, "history next 8");
    h.next = 40;
    CHECK(motion_match(&fa, &fb, &p, &h, g_pa, g_why, &g_r) == -1, "history next 40");
    h.next = 0;
    h.n = MOTION_CUT_HISTORY + 1u;
    CHECK(motion_match(&fa, &fb, &p, &h, g_pa, g_why, &g_r) == -1, "history n 9");
    h.n = 2;
    h.next = 2;
    h.step[1] = NAN;
    CHECK(motion_match(&fa, &fb, &p, &h, g_pa, g_why, &g_r) == -1, "history with a NaN step");
    h.step[1] = -1.0f;
    CHECK(motion_match(&fa, &fb, &p, &h, g_pa, g_why, &g_r) == -1, "history with a negative step");
    h.step[1] = 1.0f;
    h.next = 0;
    CHECK(motion_match(&fa, &fb, &p, &h, g_pa, g_why, &g_r) == -1, "history n 2 with next 0: the ring is not in order");
    h.next = 2;
    CHECK(motion_match(&fa, &fb, &p, &h, g_pa, g_why, &g_r) == 0, "a valid history passes");

    /* params: each field non-finite, zero or out of range */
    static const struct { int field; float v; } bad_p[] = {
        { 0, NAN }, { 0, -5.0f }, { 0, 0.0f }, { 0, INFINITY }, { 1, NAN }, { 1, 0.0f }, { 2, NAN }, { 2, -1.0f },
        { 3, NAN }, { 3, 0.0f }, { 4, NAN }, { 4, -0.1f }, { 5, NAN }, { 5, 1.5f }, { 5, -0.1f },
    };
    for (size_t k = 0; k < sizeof(bad_p) / sizeof(bad_p[0]); k++) {
        MotionMatchParams q = p;
        float *f = bad_p[k].field == 0 ? &q.d_max : bad_p[k].field == 1 ? &q.d_leaf : bad_p[k].field == 2 ? &q.cut_rotate_deg :
                   bad_p[k].field == 3 ? &q.cut_translate_k : bad_p[k].field == 4 ? &q.cut_proj_rel : &q.cut_min_coverage;
        *f = bad_p[k].v;
        CHECK(motion_match(&fa, &fb, &q, NULL, g_pa, g_why, &g_r) == -1 && g_r.flags == MOTION_MATCH_FAILED &&
              g_pa[0] == N_, "params field %d = %g: FAILED", bad_p[k].field, (double)bad_p[k].v);
    }
    MotionMatchParams q = p;
    q.cut_min_coverage = 0.0f;
    CHECK(motion_match(&fa, &fb, &q, NULL, g_pa, g_why, &g_r) == 0, "coverage 0 is valid");
    q.cut_min_coverage = 1.0f;
    CHECK(motion_match(&fa, &fb, &q, NULL, g_pa, g_why, &g_r) == 0, "coverage 1 is valid");
}

/* The GE's view transform, exactly as ge.c applies ge.view (column-major, translation at 9..11). */
static void ge_eye(const float view[12], const float w[3], float e[3]) {
    for (int a = 0; a < 3; a++)
        e[a] = view[a] * w[0] + view[3 + a] * w[1] + view[6 + a] * w[2] + view[9 + a];
}

/* A camera at world (px, 0, 0) yawed by deg about y, built in the GE's order: eye = R (w - c), so
 * view[3 c + a] = R[a][c] and view[9 + a] = -(R c)[a]. */
static void cam_set(MotionCamera *c, float px, float deg) {
    float r = deg * 3.14159265f / 180.0f, cs = cosf(r), sn = sinf(r);
    float R[3][3] = { { cs, 0, sn }, { 0, 1, 0 }, { -sn, 0, cs } }, pos[3] = { px, 0, 0 };
    for (int a = 0; a < 3; a++) {
        for (int k = 0; k < 3; k++) c->view[3 * k + a] = R[a][k];
        c->view[9 + a] = -(R[a][0] * pos[0] + R[a][1] * pos[1] + R[a][2] * pos[2]);
    }
}

static void cam_identity(MotionCamera *c) {
    memset(c, 0, sizeof(*c));
    c->valid = 1;
    cam_set(c, 0.0f, 0.0f);
    c->proj[0] = 1.5f;     /* GE order: column 0 */
    c->proj[5] = 2.6f;
    c->proj[10] = -1.0f;
    c->proj[11] = -1.0f;   /* clip w = -ez */
    c->proj[14] = -0.2f;
}

static void cam_yaw(MotionCamera *c, float deg) {
    cam_set(c, -c->view[9], deg);   /* yaw 0 keeps the position readable from view[9] */
}

static void cam_at(MotionCamera *c, float pos) {
    cam_set(c, pos, 0.0f);
}

static MotionPrim s_one[2];

static int match_cam(const MotionCamera *ca, const MotionCamera *cb, MotionCutHistory *h) {
    s_one[0] = prim(0, 0x08800000u, 10, 10);
    s_one[1] = prim(1, 0x08800100u, 50, 10);
    MotionMatchFrame fa = frame(s_one, 2), fb = frame(s_one, 2);
    fa.camera = *ca;
    fb.camera = *cb;
    MotionMatchParams p;
    motion_match_default_params(&p);
    return motion_match(&fa, &fb, &p, h, g_pa, g_why, &g_r);
}

/* The record takes ge.view as loaded. A literal GE-order identity view with the camera moved by 5
 * along x, and the fixture builder checked against ge.c's own transform. */
static void cell_ge_order(void) {
    MotionCamera ca, cb;
    cam_identity(&ca);
    cb = ca;
    static const float moved_x5[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, -5, 0, 0 };
    memcpy(cb.view, moved_x5, sizeof(moved_x5));
    CHECK(match_cam(&ca, &cb, NULL) == 0 && g_r.flags == 0 && fabsf(g_r.translate - 5.0f) < 1e-5f &&
          g_r.rotate_deg < 0.01f, "GE-order identity moved by 5: no cut, step 5 (%#x, %.3f, %.3f)", g_r.flags,
          (double)g_r.translate, (double)g_r.rotate_deg);

    /* The builder: the camera's own position maps to the eye origin, and a point one unit ahead
     * of a 20-degree yaw (forward is -z in eye space) maps to (0, 0, -1). */
    cam_set(&cb, 3.0f, 20.0f);
    float r = 20.0f * 3.14159265f / 180.0f, e[3];
    const float c[3] = { 3, 0, 0 }, ahead[3] = { 3.0f + sinf(r), 0.0f, -cosf(r) };
    ge_eye(cb.view, c, e);
    CHECK(fabsf(e[0]) < 1e-5f && fabsf(e[1]) < 1e-5f && fabsf(e[2]) < 1e-5f, "fixture: the camera is the eye origin");
    ge_eye(cb.view, ahead, e);
    CHECK(fabsf(e[0]) < 1e-5f && fabsf(e[1]) < 1e-5f && fabsf(e[2] + 1.0f) < 1e-5f, "fixture: ahead is -z (%f %f %f)",
          (double)e[0], (double)e[1], (double)e[2]);
    CHECK(match_cam(&ca, &cb, NULL) == 0 && g_r.flags == 0 && fabsf(g_r.rotate_deg - 20.0f) < 0.05f &&
          fabsf(g_r.translate - 3.0f) < 1e-4f, "GE-order 20-degree yaw at x 3 (%#x, %.3f, %.3f)", g_r.flags,
          (double)g_r.rotate_deg, (double)g_r.translate);

    /* Turning in place away from the origin: the position is -R^-1 t with R as the GE stores it, so
     * a transposed rotation would report a step (6.84 here) where there is none. */
    cam_set(&ca, 10.0f, 0.0f);
    cam_set(&cb, 10.0f, 20.0f);
    CHECK(match_cam(&ca, &cb, NULL) == 0 && g_r.flags == 0 && fabsf(g_r.rotate_deg - 20.0f) < 0.05f &&
          g_r.translate < 1e-4f, "a 20-degree turn in place at x 10 is no step (%#x, %.3f, %.4f)", g_r.flags,
          (double)g_r.rotate_deg, (double)g_r.translate);
}

static void cell_cut_rotate(void) {
    MotionCamera ca, cb;
    cam_identity(&ca);
    cb = ca;
    cam_yaw(&cb, 20.0f);
    CHECK(match_cam(&ca, &cb, NULL) == 0 && g_r.flags == 0 && fabsf(g_r.rotate_deg - 20.0f) < 0.05f,
          "20 degrees is no cut (%#x, %.3f)", g_r.flags, (double)g_r.rotate_deg);
    cam_yaw(&cb, 45.0f);
    CHECK(match_cam(&ca, &cb, NULL) == 0 && g_r.flags == MOTION_MATCH_CUT_ROTATE && fabsf(g_r.rotate_deg - 45.0f) < 0.05f,
          "45 degrees cuts (%#x, %.3f)", g_r.flags, (double)g_r.rotate_deg);
    CHECK(g_pa[0] == 0 && g_pa[1] == 1, "a cut frame still reports its pairs");
    /* a uniformly scaled view is the same rotation */
    for (int k = 0; k < 12; k++) cb.view[k] *= 3.0f;
    CHECK(match_cam(&ca, &cb, NULL) == 0 && fabsf(g_r.rotate_deg - 45.0f) < 0.05f, "scale is not rotation (%.3f)",
          (double)g_r.rotate_deg);
}

static void cell_cut_translate(void) {
    MotionCamera ca, cb;
    MotionCutHistory h;
    memset(&h, 0, sizeof(h));
    cam_identity(&ca);
    cb = ca;
    /* before four steps of history, any step is allowed */
    cam_at(&cb, 100.0f);
    CHECK(match_cam(&ca, &cb, &h) == 0 && g_r.flags == 0 && fabsf(g_r.translate - 100.0f) < 1e-3f &&
          g_r.translate_median == 0.0f, "no history: no translation cut");
    memset(&h, 0, sizeof(h));
    for (int k = 0; k < 4; k++) {
        cam_at(&ca, (float)k);
        cam_at(&cb, (float)(k + 1));
        CHECK(match_cam(&ca, &cb, &h) == 0 && g_r.flags == 0, "steady step %d", k);
    }
    CHECK(h.n == 4, "four steps recorded (%u)", h.n);
    cam_at(&ca, 4.0f);
    cam_at(&cb, 11.0f);   /* 7 <= 8 x 1 */
    CHECK(match_cam(&ca, &cb, &h) == 0 && g_r.flags == 0 && fabsf(g_r.translate_median - 1.0f) < 1e-5f,
          "7 times the median is no cut (%#x, %.3f)", g_r.flags, (double)g_r.translate_median);
    cam_at(&ca, 11.0f);
    cam_at(&cb, 21.0f);   /* 10 > 8 x median(1, 1, 1, 1, 7), which is 1 */
    CHECK(match_cam(&ca, &cb, &h) == 0 && g_r.flags == MOTION_MATCH_CUT_TRANSLATE, "10 times cuts (%#x)", g_r.flags);
    CHECK(h.n == 5, "a cut step is not recorded (%u)", h.n);

    /* The median, not the minimum or the mean: steps 1, 2, 3, 10 have median 2.5 (min 1, mean 4). */
    memset(&h, 0, sizeof(h));
    static const float steps[4] = { 1, 2, 3, 10 };
    float at = 0.0f;
    for (int k = 0; k < 4; k++) {
        cam_at(&ca, at);
        cam_at(&cb, at + steps[k]);
        CHECK(match_cam(&ca, &cb, &h) == 0 && g_r.flags == 0, "uneven step %d", k);
        at += steps[k];
    }
    cam_at(&ca, at);
    cam_at(&cb, at + 19.0f);   /* 19 <= 8 x 2.5 = 20, but above 8 x the minimum */
    CHECK(match_cam(&ca, &cb, &h) == 0 && g_r.flags == 0 && fabsf(g_r.translate_median - 2.5f) < 1e-4f,
          "19 against median 2.5 is no cut (%#x, %.3f)", g_r.flags, (double)g_r.translate_median);
    memset(&h, 0, sizeof(h));
    at = 0.0f;
    for (int k = 0; k < 4; k++) {
        cam_at(&ca, at);
        cam_at(&cb, at + steps[k]);
        (void)match_cam(&ca, &cb, &h);
        at += steps[k];
    }
    cam_at(&ca, at);
    cam_at(&cb, at + 21.0f);   /* 21 > 20, but under 8 x the mean (32) */
    CHECK(match_cam(&ca, &cb, &h) == 0 && g_r.flags == MOTION_MATCH_CUT_TRANSLATE, "21 against median 2.5 cuts (%#x)",
          g_r.flags);

    /* a static camera that starts to move is not a cut */
    memset(&h, 0, sizeof(h));
    cam_at(&ca, 0.0f);
    cam_at(&cb, 0.0f);
    for (int k = 0; k < 8; k++) CHECK(match_cam(&ca, &cb, &h) == 0 && g_r.flags == 0, "static %d", k);
    cam_at(&cb, 5.0f);
    CHECK(match_cam(&ca, &cb, &h) == 0 && g_r.flags == 0 && g_r.translate_median == 0.0f,
          "starting to move is no cut (%#x)", g_r.flags);
    /* without a history, the rule is off */
    cam_at(&cb, 1e6f);
    CHECK(match_cam(&ca, &cb, NULL) == 0 && g_r.flags == 0, "no history: off");
}

static void cell_cut_proj(void) {
    MotionCamera ca, cb;
    cam_identity(&ca);
    cb = ca;
    cb.proj[0] = 1.5f * 1.05f;
    CHECK(match_cam(&ca, &cb, NULL) == 0 && g_r.flags == 0, "5%% zoom is no cut (%#x)", g_r.flags);
    cb.proj[1] = 1e-5f;   /* below 1e-3 of the largest entry: ignored */
    CHECK(match_cam(&ca, &cb, NULL) == 0 && g_r.flags == 0, "a tiny entry is ignored (%#x)", g_r.flags);
    cb.proj[0] = 1.5f * 1.2f;
    CHECK(match_cam(&ca, &cb, NULL) == 0 && g_r.flags == MOTION_MATCH_CUT_PROJ, "20%% zoom cuts (%#x)", g_r.flags);
}

static void cell_cut_camera(void) {
    MotionCamera ca, cb;
    cam_identity(&ca);
    memset(&cb, 0, sizeof(cb));
    CHECK(match_cam(&ca, &cb, NULL) == 0 && g_r.flags == MOTION_MATCH_CUT_CAMERA, "camera lost (%#x)", g_r.flags);
    CHECK(match_cam(&cb, &ca, NULL) == 0 && g_r.flags == MOTION_MATCH_CUT_CAMERA, "camera appears (%#x)", g_r.flags);
    CHECK(match_cam(&cb, &cb, NULL) == 0 && g_r.flags == 0, "no camera in either: no rule (%#x)", g_r.flags);
    cb = ca;
    memset(cb.view, 0, sizeof(cb.view));
    CHECK(match_cam(&ca, &cb, NULL) == 0 && g_r.flags == MOTION_MATCH_CUT_CAMERA, "a singular view cuts (%#x)", g_r.flags);
}

/* A non-finite camera fails closed: a cut, and nothing enters the history. */
static void cell_camera_non_finite(void) {
    MotionCamera ca, cb;
    MotionCutHistory h;
    memset(&h, 0, sizeof(h));
    cam_identity(&ca);
    for (int k = 0; k < 4; k++) {
        cam_at(&ca, (float)k);
        cb = ca;
        cam_at(&cb, (float)(k + 1));
        (void)match_cam(&ca, &cb, &h);
    }
    CHECK(h.n == 4, "non-finite: four steps first (%u)", h.n);
    cb = ca;
    cb.view[9] = NAN;
    CHECK(match_cam(&ca, &cb, &h) == 0 && g_r.flags == MOTION_MATCH_CUT_CAMERA && h.n == 4,
          "a NaN translation cuts and is not recorded (%#x, n %u)", g_r.flags, h.n);
    cb.view[9] = INFINITY;
    CHECK(match_cam(&ca, &cb, &h) == 0 && g_r.flags == MOTION_MATCH_CUT_CAMERA && h.n == 4,
          "an infinite translation cuts (%#x, n %u)", g_r.flags, h.n);
    cb = ca;
    cb.proj[5] = NAN;
    CHECK(match_cam(&ca, &cb, &h) == 0 && g_r.flags == MOTION_MATCH_CUT_CAMERA && h.n == 4,
          "a NaN projection entry cuts (%#x)", g_r.flags);
    cb = ca;
    ca.proj[14] = -INFINITY;
    CHECK(match_cam(&ca, &cb, &h) == 0 && g_r.flags == MOTION_MATCH_CUT_CAMERA, "an infinite projection in A cuts (%#x)",
          g_r.flags);

    /* Finite entries whose pose overflows: scale 1e-6 and a translation of 1e33 put the camera at
     * 1e39, beyond a float. A cut, with nothing recorded. */
    cam_identity(&ca);
    cb = ca;
    memset(cb.view, 0, sizeof(cb.view));
    cb.view[0] = cb.view[4] = cb.view[8] = 1e-6f;
    cb.view[9] = 1e33f;
    uint32_t n0 = h.n;
    CHECK(match_cam(&ca, &cb, &h) == 0 && g_r.flags == MOTION_MATCH_CUT_CAMERA && h.n == n0 && isfinite(g_r.translate),
          "an overflowing pose cuts (%#x, %f, n %u)", g_r.flags, (double)g_r.translate, h.n);
    /* and a finite pose whose step overflows */
    cam_set(&ca, -3e38f, 0.0f);
    cam_set(&cb, 3e38f, 0.0f);
    CHECK(match_cam(&ca, &cb, &h) == 0 && g_r.flags == MOTION_MATCH_CUT_CAMERA && h.n == n0,
          "an overflowing step cuts (%#x, n %u)", g_r.flags, h.n);
}

static void cell_cut_coverage(void) {
    MotionPrim a[1] = { prim(0, 0x08800000u, 0, 0) }, b[3];
    a[0].count = 30;
    b[0] = a[0];
    b[1] = prim(4, 0x08800400u, 0, 0);
    b[1].count = 40;   /* 30 of 70 matched */
    CHECK(match(a, 1, b, 2) == 0 && g_r.flags == MOTION_MATCH_CUT_COVERAGE && g_r.xform_verts == 70 &&
          g_r.xform_verts_matched == 30, "30 of 70 cuts (%#x)", g_r.flags);
    b[1].count = 20;   /* 30 of 50 */
    CHECK(match(a, 1, b, 2) == 0 && g_r.flags == 0, "30 of 50 is no cut (%#x)", g_r.flags);
    b[1].count = 400;
    b[1].through = 1;  /* through-mode vertices are not the camera's */
    b[2] = prim(5, 0x08800500u, 0, 0);
    b[2].through = 1;
    CHECK(match(a, 1, b, 3) == 0 && g_r.flags == 0 && g_r.xform_verts == 30, "through mode is not counted (%#x)", g_r.flags);
}

/* ---- randomized: invariants, the brute-force longest run, determinism -------------------- */

static uint32_t s_rng = 0x2081u;

static uint32_t rnd(uint32_t n) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return n ? s_rng % n : 0u;
}

static int sig_same(const MotionPrim *x, const MotionPrim *y) {
    return x->through == y->through && x->prim_type == y->prim_type && x->vtype == y->vtype && x->count == y->count &&
           x->tex_addr == y->tex_addr && x->clut_addr == y->clut_addr && x->state_class == y->state_class;
}

/* Unique keys in a random permutation: every PRIM pairs, and the anchors are a longest increasing
 * run, of the length an O(n^2) search finds. */
static void cell_random_permutation(void) {
    for (int t = 0; t < 300; t++) {
        uint32_t n = 1u + rnd(48);
        MotionPrim a[48], b[48];
        uint32_t perm[48];
        for (uint32_t k = 0; k < n; k++) {
            a[k] = prim(rnd(3), 0x08800000u + k * 0x20u, 0, 0);
            perm[k] = k;
        }
        for (uint32_t k = n; k-- > 1;) {
            uint32_t r = rnd(k + 1u), x = perm[k];
            perm[k] = perm[r];
            perm[r] = x;
        }
        for (uint32_t j = 0; j < n; j++) b[j] = a[perm[j]];
        uint32_t lis[48], best = 0;
        for (uint32_t j = 0; j < n; j++) {
            lis[j] = 1;
            for (uint32_t q = 0; q < j; q++)
                if (perm[q] < perm[j] && lis[q] + 1u > lis[j]) lis[j] = lis[q] + 1u;
            if (lis[j] > best) best = lis[j];
        }
        CHECK(match(a, n, b, n) == 0, "perm %d: returns 0", t);
        int32_t last = -1;
        int all = 1;
        for (uint32_t j = 0; j < n; j++) {
            all &= g_pa[j] == (int32_t)perm[j];
            if (g_why[j] == AN) {
                CHECK(g_pa[j] > last, "perm %d: anchors in order", t);
                last = g_pa[j];
            }
        }
        CHECK(all, "perm %d: every PRIM pairs with itself", t);
        CHECK(g_r.anchors == best && g_r.anchors + g_r.reorders == n, "perm %d (n %u): %u anchors, longest run %u", t, n,
              g_r.anchors, best);
    }
}

static void random_frame(MotionPrim *p, uint32_t n) {
    for (uint32_t k = 0; k < n; k++) {
        p[k] = prim(rnd(5), 0x08800000u + rnd(8) * 0x40u, (float)rnd(480), (float)rnd(272));
        p[k].through = (uint8_t)(rnd(4) == 0);
        if (rnd(6) == 0) p[k].count = 3u + rnd(3) * 3u;
    }
}

static void cell_random_invariants(void) {
    static MotionPrim a[160], b[160];
    static int32_t pa2[160];
    static uint8_t why2[160];
    MotionMatchParams p;
    motion_match_default_params(&p);
    for (int t = 0; t < 400; t++) {
        uint32_t na = rnd(160), nb = rnd(160);
        random_frame(a, na);
        if (t & 1) {
            /* B from A: small moves, some deletions, insertions and swaps */
            uint32_t m = 0;
            for (uint32_t k = 0; k < na && m < 160; k++) {
                if (rnd(10) == 0) continue;
                if (rnd(10) == 0 && m < 159) random_frame(&b[m++], 1);
                b[m++] = moved(a[k], (float)rnd(9) - 4.0f, (float)rnd(9) - 4.0f);
            }
            for (uint32_t s = 0; m > 1 && s < 3; s++) {
                uint32_t x = rnd(m), y = rnd(m);
                MotionPrim q = b[x];
                b[x] = b[y];
                b[y] = q;
            }
            nb = m;
        } else {
            random_frame(b, nb);
        }
        MotionMatchFrame fa = frame(a, na), fb = frame(b, nb);
        CHECK(motion_match(&fa, &fb, &p, NULL, g_pa, g_why, &g_r) == 0, "random %d: returns 0", t);
        MotionMatchResult r1 = g_r;
        uint8_t used[160];
        memset(used, 0, sizeof(used));
        uint32_t anchors = 0, fills = 0, reorders = 0, teleports = 0, unmatched = 0;
        for (uint32_t j = 0; j < nb; j++) {
            int32_t i = g_pa[j];
            if (i >= 0) {
                CHECK((uint32_t)i < na && !used[i], "random %d: B[%u] has a valid, unshared partner", t, j);
                if ((uint32_t)i >= na) continue;
                used[i] = 1;
                CHECK(sig_same(&a[i], &b[j]), "random %d: B[%u] pairs only within its signature", t, j);
                float dx = 0.5f * (b[j].bbox[0] + b[j].bbox[2] - a[i].bbox[0] - a[i].bbox[2]);
                float dy = 0.5f * (b[j].bbox[1] + b[j].bbox[3] - a[i].bbox[1] - a[i].bbox[3]);
                CHECK(dx * dx + dy * dy <= p.d_max * p.d_max, "random %d: B[%u] pair is plausible", t, j);
                CHECK(g_why[j] == AN || g_why[j] == FI || g_why[j] == RE, "random %d: B[%u] paired reason", t, j);
                anchors += g_why[j] == AN;
                fills += g_why[j] == FI;
                reorders += g_why[j] == RE;
            } else {
                CHECK(g_why[j] == NO || g_why[j] == TP, "random %d: B[%u] unpaired reason", t, j);
                teleports += g_why[j] == TP;
                unmatched += g_why[j] == NO;
            }
        }
        if (na)
            CHECK(r1.anchors == anchors && r1.fills == fills && r1.reorders == reorders && r1.teleports == teleports &&
                  r1.unmatched == unmatched, "random %d: the counts match the reasons", t);
        /* the same input gives the same answer, with or without reasons asked for */
        CHECK(motion_match(&fa, &fb, &p, NULL, pa2, why2, &g_r) == 0 && memcmp(pa2, g_pa, nb * sizeof(int32_t)) == 0 &&
              memcmp(why2, g_why, nb) == 0 && memcmp(&r1, &g_r, sizeof(r1)) == 0, "random %d: deterministic", t);
        CHECK(motion_match(&fa, &fb, &p, NULL, pa2, NULL, &g_r) == 0 && memcmp(pa2, g_pa, nb * sizeof(int32_t)) == 0 &&
              memcmp(&r1, &g_r, sizeof(r1)) == 0, "random %d: the same without reasons", t);
    }
}

int main(void) {
    cell_identical();
    cell_reorder();
    cell_longest_run();
    cell_spawn_despawn();
    cell_rebuild();
    cell_ties();
    cell_spawn_in_run();
    cell_lcs_leaf();
    cell_lcs_not_nearest();
    cell_lcs_tie();
    cell_key_unique_in_a();
    cell_gap_sig_repeated_in_a();
    cell_inline_never_anchors();
    cell_exact_bounds();
    cell_large_leaf();
    cell_large_leaf_nearest();
    cell_nested_gap();
    cell_count_change();
    cell_teleport();
    cell_no_previous();
    cell_failed();
    cell_ge_order();
    cell_cut_rotate();
    cell_cut_translate();
    cell_cut_proj();
    cell_cut_camera();
    cell_camera_non_finite();
    cell_cut_coverage();
    cell_random_permutation();
    cell_random_invariants();
    if (s_fail) {
        fprintf(stderr, "motion_match_selftest: %d failure(s)\n", s_fail);
        return 1;
    }
    printf("motion_match_selftest: PASS\n");
    return 0;
}
