/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * motion_match.h - the draw matcher of render-state motion (#2081): pairs each PRIM of frame B
 * with the PRIM of frame A that drew the same object, so a later pass can take each vertex's
 * previous position for motion vectors and in-between frames.
 *
 * Pure C: no Vulkan, no runtime calls, no globals. The caller (the journal, motion PR 2c) fills
 * one MotionPrim per display-target PRIM in draw order, and keeps a MotionCutHistory per display
 * target across frames. The rule, in order:
 *
 *   1. Signature: through mode, PRIM type, vtype, vertex count, texture address, CLUT address and
 *      the blend/depth class. Only PRIMs with equal signatures can pair.
 *   2. Anchors: a PRIM whose key (signature plus vertex and index addresses) occurs exactly once in
 *      A and once in B. The longest run of anchors in the same order in both frames is kept. That
 *      is the patience-diff rule, and the same rule the #2091 census measures (with the census's
 *      shorter signature).
 *   3. Fill: between consecutive anchors, equal signatures pair in order. Within a gap, signatures
 *      unique to that gap in both frames anchor it (patience recursion); the leaves pair a common
 *      prefix and suffix, then an exact longest common subsequence when the leaf is small, else in
 *      order with a bounded lookahead. In a leaf a pair must also pass step 5's distance, so one
 *      spawn in a run of equal signatures (particles, glyphs) does not shift every later pair.
 *   4. Reorder: an anchor-quality key left out of the ordered run (a draw that moved in the list)
 *      still pairs. It is applied before the fill, so the fill cannot take its partner.
 *   5. Plausibility: a pair whose bounding-box centre moved more than d_max guest pixels is dropped
 *      (a teleport). That PRIM is unmatched: drawn as in B.
 *   6. Cut, for the whole frame: the camera rotated more than cut_rotate_deg, or moved more than
 *      cut_translate_k times its recent median step, or the projection jumped, or under
 *      cut_min_coverage of B's transform-mode vertices are matched.
 *
 * A consumer treats a cut frame as having no pairs; the pairs are still reported for the census.
 */
#ifndef SR_MOTION_MATCH_H
#define SR_MOTION_MATCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One display-target PRIM, as the tap records it. */
typedef struct {
    uint8_t  through;          /* 1: through mode (pre-transformed 2D) */
    uint8_t  prim_type;        /* GE PRIM type, 0..7 */
    uint16_t reserved;         /* 0 */
    uint32_t vtype;            /* GE vertex type, including the index format */
    uint32_t count;            /* vertex count of the PRIM command */
    uint32_t tex_addr;         /* 0 when untextured */
    uint32_t clut_addr;        /* 0 when the texture has no CLUT */
    uint32_t state_class;      /* blend and depth class bits, chosen by the tap */
    uint32_t vaddr;            /* vertex source address */
    uint32_t iaddr;            /* index source address, 0 when not indexed */
    float    bbox[4];          /* x0, y0, x1, y1 of its submitted vertices, guest pixels */
} MotionPrim;

/* The camera of a frame: the view and projection used by most transform-mode vertices. */
typedef struct {
    int   valid;               /* 0: the frame has no transform-mode PRIM */
    float view[12];            /* GE 3x4 view matrix, row-major: rows are x, y, z axes plus a translation column */
    float proj[16];            /* GE 4x4 projection matrix, as loaded */
} MotionCamera;

typedef struct {
    const MotionPrim *prims;
    uint32_t nprims;
    MotionCamera camera;
} MotionMatchFrame;

typedef struct {
    float d_max;               /* teleport bound, guest pixels per frame; default 1/8 of 480x272's diagonal */
    float cut_rotate_deg;      /* default 30 */
    float cut_translate_k;     /* default 8: a step above k times the recent median step is a cut */
    float cut_proj_rel;        /* default 0.1: a projection entry changing by more than this share */
    float cut_min_coverage;    /* default 0.5: share of B's transform-mode vertices that must match */
} MotionMatchParams;

#define MOTION_CUT_HISTORY 8u

/* Recent camera steps of one display target, kept by the caller across frames. Zero it to start. */
typedef struct {
    float    step[MOTION_CUT_HISTORY];
    uint32_t n;                /* steps recorded, at most MOTION_CUT_HISTORY */
    uint32_t next;
} MotionCutHistory;

/* Why a PRIM of B has, or has not, a partner. */
typedef enum {
    MOTION_PAIR_NONE = 0,      /* no PRIM of A has its signature, or every candidate was taken */
    MOTION_PAIR_ANCHOR,        /* step 2 */
    MOTION_PAIR_FILL,          /* step 3 */
    MOTION_PAIR_REORDER,       /* step 4 */
    MOTION_PAIR_TELEPORT       /* step 5: a partner was found and dropped */
} MotionPairWhy;

/* Frame flags. */
#define MOTION_MATCH_NO_PREVIOUS   0x01u  /* A is empty: nothing to pair with */
#define MOTION_MATCH_CUT_ROTATE    0x02u
#define MOTION_MATCH_CUT_TRANSLATE 0x04u
#define MOTION_MATCH_CUT_PROJ      0x08u
#define MOTION_MATCH_CUT_COVERAGE  0x10u
#define MOTION_MATCH_CUT_CAMERA    0x20u  /* one frame has a camera and the other has none */
#define MOTION_MATCH_FAILED        0x40u  /* invalid argument or out of memory: nothing was paired */
#define MOTION_MATCH_CUT (MOTION_MATCH_CUT_ROTATE | MOTION_MATCH_CUT_TRANSLATE | MOTION_MATCH_CUT_PROJ | \
                          MOTION_MATCH_CUT_COVERAGE | MOTION_MATCH_CUT_CAMERA)

typedef struct {
    uint32_t flags;
    uint32_t anchors, fills, reorders, teleports, unmatched;
    uint64_t xform_verts, xform_verts_matched;   /* vertex mass of B's transform-mode PRIMs */
    float    rotate_deg;       /* camera rotation A to B, 0 without both cameras */
    float    translate;        /* camera position step A to B, in the title's world units */
    float    translate_median; /* the recent median step it was judged against, 0 before history */
} MotionMatchResult;

void motion_match_default_params(MotionMatchParams *p);

/* Pairs B with A. pair_a[j] receives the index in A of B's PRIM j, or -1; why[j] (may be NULL)
 * receives the reason. hist (may be NULL: no translation rule) is updated with this frame's
 * camera step. Returns 0, or -1 when an argument is invalid or memory runs out; then every
 * pair_a[j] is -1 (when pair_a is given) and the result's flags say FAILED, so a failure never
 * pairs anything and the caller can count it. */
int motion_match(const MotionMatchFrame *a, const MotionMatchFrame *b, const MotionMatchParams *p,
                 MotionCutHistory *hist, int32_t *pair_a, uint8_t *why, MotionMatchResult *out);

#ifdef __cplusplus
}
#endif

#endif /* SR_MOTION_MATCH_H */
