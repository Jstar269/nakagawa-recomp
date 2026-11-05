/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * motion_match.c - the draw matcher of render-state motion (#2081). The rule is in motion_match.h.
 * Pure C: the C library only.
 */
#include "motion_match.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- keys ------------------------------------------------------------------------------------ */

static int sig_eq(const MotionPrim *x, const MotionPrim *y) {
    return x->through == y->through && x->prim_type == y->prim_type && x->vtype == y->vtype &&
           x->count == y->count && x->tex_addr == y->tex_addr && x->clut_addr == y->clut_addr &&
           x->state_class == y->state_class;
}

static int key_eq(const MotionPrim *x, const MotionPrim *y) {
    return sig_eq(x, y) && x->vaddr == y->vaddr && x->iaddr == y->iaddr;
}

static uint64_t fnv_u32(uint64_t h, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        h ^= (v >> (8 * i)) & 0xFFu;
        h *= 0x100000001B3ull;
    }
    return h;
}

static uint64_t sig_hash(const MotionPrim *p) {
    uint64_t h = 0xCBF29CE484222325ull;
    h = fnv_u32(h, (uint32_t)p->through | ((uint32_t)p->prim_type << 8));
    h = fnv_u32(h, p->vtype);
    h = fnv_u32(h, p->count);
    h = fnv_u32(h, p->tex_addr);
    h = fnv_u32(h, p->clut_addr);
    h = fnv_u32(h, p->state_class);
    return h;
}

static uint64_t key_hash(const MotionPrim *p) {
    return fnv_u32(fnv_u32(sig_hash(p), p->vaddr), p->iaddr);
}

typedef struct {
    uint64_t h;
    uint32_t idx;
} HashIdx;

static int cmp_hash_idx(const void *x, const void *y) {
    const HashIdx *a = x, *b = y;
    if (a->h != b->h) return a->h < b->h ? -1 : 1;
    return a->idx < b->idx ? -1 : (a->idx > b->idx);
}

typedef struct {
    int32_t a, b;
} Pair;

static int cmp_pair_a(const void *x, const void *y) {
    const Pair *p = x, *q = y;
    if (p->a != q->a) return p->a < q->a ? -1 : 1;
    return p->b < q->b ? -1 : (p->b > q->b);
}

/* Occurrences of prims[k]'s key (eq) among the run of `sorted` with hash h; *first gets the
 * lowest index with that exact key. `sorted` is sorted by (hash, idx). */
static uint32_t count_exact(const HashIdx *sorted, uint32_t n, uint64_t h, const MotionPrim *prims,
                            const MotionPrim *probe, int (*eq)(const MotionPrim *, const MotionPrim *),
                            int32_t *first) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (sorted[mid].h < h) lo = mid + 1u;
        else hi = mid;
    }
    uint32_t c = 0;
    *first = -1;
    for (uint32_t k = lo; k < n && sorted[k].h == h; k++)
        if (eq(&prims[sorted[k].idx], probe)) {
            if (c == 0) *first = (int32_t)sorted[k].idx;
            c++;
        }
    return c;
}

/* Longest run of pairs (sorted by a) whose b strictly increases; written back into p, returns its
 * length. Scratch: tails, tails_i and prev, each n entries. */
static uint32_t longest_increasing(Pair *p, uint32_t n, int32_t *tails, uint32_t *tails_i, int32_t *prev) {
    uint32_t len = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t lo = 0, hi = len;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2u;
            if (tails[mid] < p[i].b) lo = mid + 1u;
            else hi = mid;
        }
        tails[lo] = p[i].b;
        tails_i[lo] = i;
        prev[i] = lo ? (int32_t)tails_i[lo - 1u] : -1;
        if (lo == len) len++;
    }
    if (len == 0) return 0;
    /* Collect the chain's indices (they increase along the run, and the k-th is at least k), then
     * copy forward: each write lands at or before the index it reads, and before every later read. */
    int32_t k = (int32_t)tails_i[len - 1u];
    for (uint32_t w = len; w-- > 0;) {
        tails_i[w] = (uint32_t)k;
        k = prev[k];
    }
    for (uint32_t w = 0; w < len; w++) p[w] = p[tails_i[w]];
    return len;
}

/* ---- the fill ------------------------------------------------------------------------------- */

#define LEAF_LCS_MAX 64u        /* exact LCS for leaves up to 64 x 64 */
#define GREEDY_LOOKAHEAD 64u    /* in-order pairing looks this far ahead in A */

typedef struct {
    const MotionPrim *a, *b;
    int32_t *pair_a;            /* B index -> A index */
    uint8_t *why;               /* B index -> MotionPairWhy */
    uint8_t *used_a;
    /* scratch, each sized for the larger frame */
    int32_t *ga, *gb;           /* unclaimed members of the current gap */
    HashIdx *ha, *hb;
    Pair *cand;
    int32_t *tails, *prev;
    uint32_t *tails_i;
    int32_t (*stack)[4];        /* gaps waiting: a0, a1, b0, b1 (half-open) */
    uint32_t nstack, cap_stack;
    uint16_t *lcs;              /* (LEAF_LCS_MAX + 1)^2 */
    float dl2;                  /* d_leaf squared */
} Fill;

static float centre_dist2(const MotionPrim *x, const MotionPrim *y) {
    float dx = 0.5f * ((y->bbox[0] + y->bbox[2]) - (x->bbox[0] + x->bbox[2]));
    float dy = 0.5f * ((y->bbox[1] + y->bbox[3]) - (x->bbox[1] + x->bbox[3]));
    return dx * dx + dy * dy;
}

/* In a leaf, a run of equal signatures carries no order information of its own (particles,
 * glyphs, a buffer rebuilt at a new address), so a pair there must also be within d_leaf: one
 * spawn in the run then does not shift every later pair by one. */
static int leaf_pairs(const Fill *f, int32_t ia, int32_t jb) {
    return sig_eq(&f->a[ia], &f->b[jb]) && centre_dist2(&f->a[ia], &f->b[jb]) <= f->dl2;
}

static void pair_up(Fill *f, int32_t ia, int32_t jb, uint8_t why) {
    f->pair_a[jb] = ia;
    f->used_a[ia] = 1;
    if (f->why) f->why[jb] = why;
}

static int push_gap(Fill *f, int32_t a0, int32_t a1, int32_t b0, int32_t b1) {
    if (a1 <= a0 || b1 <= b0) return 1;
    if (f->nstack == f->cap_stack) return 0;
    f->stack[f->nstack][0] = a0;
    f->stack[f->nstack][1] = a1;
    f->stack[f->nstack][2] = b0;
    f->stack[f->nstack][3] = b1;
    f->nstack++;
    return 1;
}

/* A leaf: no signature is unique to the gap in both frames. ga[0..la) and gb[0..lb) are the
 * unclaimed members in draw order. */
static void fill_leaf(Fill *f, uint32_t la, uint32_t lb) {
    uint32_t s = 0;
    while (s < la && s < lb && leaf_pairs(f, f->ga[s], f->gb[s])) {
        pair_up(f, f->ga[s], f->gb[s], MOTION_PAIR_FILL);
        s++;
    }
    uint32_t e = 0;
    while (e < la - s && e < lb - s && leaf_pairs(f, f->ga[la - 1u - e], f->gb[lb - 1u - e])) {
        pair_up(f, f->ga[la - 1u - e], f->gb[lb - 1u - e], MOTION_PAIR_FILL);
        e++;
    }
    uint32_t ma = la - s - e, mb = lb - s - e;
    const int32_t *ga = f->ga + s, *gb = f->gb + s;
    if (ma == 0 || mb == 0) return;
    if (ma <= LEAF_LCS_MAX && mb <= LEAF_LCS_MAX) {
        /* exact longest common subsequence; on a tie the backtrack skips the A member, so the
         * earlier B keeps its partner */
        uint32_t w = mb + 1u;
        uint16_t *t = f->lcs;
        for (uint32_t i = 0; i <= ma; i++) t[i * w + mb] = 0;
        for (uint32_t j = 0; j <= mb; j++) t[ma * w + j] = 0;
        for (uint32_t i = ma; i-- > 0;)
            for (uint32_t j = mb; j-- > 0;) {
                if (leaf_pairs(f, ga[i], gb[j])) t[i * w + j] = (uint16_t)(t[(i + 1u) * w + j + 1u] + 1u);
                else {
                    uint16_t d = t[(i + 1u) * w + j], r = t[i * w + j + 1u];
                    t[i * w + j] = d > r ? d : r;
                }
            }
        uint32_t i = 0, j = 0;
        while (i < ma && j < mb) {
            if (leaf_pairs(f, ga[i], gb[j]) && t[i * w + j] == t[(i + 1u) * w + j + 1u] + 1u) {
                pair_up(f, ga[i], gb[j], MOTION_PAIR_FILL);
                i++;
                j++;
            } else if (t[(i + 1u) * w + j] >= t[i * w + j + 1u]) {
                i++;
            } else {
                j++;
            }
        }
        return;
    }
    /* large leaf: in order, each B takes the nearest plausible A with its signature within the
     * lookahead; the A members it skipped stay unpaired, so the order is kept */
    uint32_t i = 0;
    for (uint32_t j = 0; j < mb && i < ma; j++) {
        uint32_t end = i + GREEDY_LOOKAHEAD < ma ? i + GREEDY_LOOKAHEAD : ma, best = end;
        float bd = 0.0f;
        for (uint32_t k = i; k < end; k++)
            if (leaf_pairs(f, ga[k], gb[j])) {
                float d = centre_dist2(&f->a[ga[k]], &f->b[gb[j]]);
                if (best == end || d < bd) {
                    best = k;
                    bd = d;
                }
            }
        if (best < end) {
            pair_up(f, ga[best], gb[j], MOTION_PAIR_FILL);
            i = best + 1u;
        }
    }
}

/* One gap [a0, a1) x [b0, b1): signatures unique to the gap in both frames anchor it, and the
 * sub-gaps between them are pushed; with none, it is a leaf. */
static int fill_gap(Fill *f, int32_t a0, int32_t a1, int32_t b0, int32_t b1) {
    uint32_t la = 0, lb = 0;
    for (int32_t i = a0; i < a1; i++)
        if (!f->used_a[i]) f->ga[la++] = i;
    for (int32_t j = b0; j < b1; j++)
        if (f->pair_a[j] < 0) f->gb[lb++] = j;
    if (la == 0 || lb == 0) return 1;
    for (uint32_t k = 0; k < la; k++) f->ha[k] = (HashIdx){ sig_hash(&f->a[f->ga[k]]), k };
    for (uint32_t k = 0; k < lb; k++) f->hb[k] = (HashIdx){ sig_hash(&f->b[f->gb[k]]), k };
    qsort(f->ha, la, sizeof(HashIdx), cmp_hash_idx);
    qsort(f->hb, lb, sizeof(HashIdx), cmp_hash_idx);
    uint32_t nc = 0;
    for (uint32_t k = 0; k < lb; k++) {
        const MotionPrim *pb = &f->b[f->gb[f->hb[k].idx]];
        /* unique in B's gap? the run of equal hashes is short; compare exactly */
        uint32_t cb = 0;
        for (uint32_t q = k; q-- > 0 && f->hb[q].h == f->hb[k].h;)
            if (sig_eq(&f->b[f->gb[f->hb[q].idx]], pb)) cb++;
        for (uint32_t q = k + 1u; q < lb && f->hb[q].h == f->hb[k].h; q++)
            if (sig_eq(&f->b[f->gb[f->hb[q].idx]], pb)) cb++;
        if (cb) continue;
        /* the gap's A members are in ga order; translate to a probe over a */
        uint32_t lo = 0, hi = la, ca = 0, at = 0;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2u;
            if (f->ha[mid].h < f->hb[k].h) lo = mid + 1u;
            else hi = mid;
        }
        for (uint32_t q = lo; q < la && f->ha[q].h == f->hb[k].h; q++)
            if (sig_eq(&f->a[f->ga[f->ha[q].idx]], pb)) {
                ca++;
                at = f->ha[q].idx;
            }
        if (ca == 1) f->cand[nc++] = (Pair){ (int32_t)at, (int32_t)f->hb[k].idx };
    }
    if (nc == 0) {
        fill_leaf(f, la, lb);
        return 1;
    }
    qsort(f->cand, nc, sizeof(Pair), cmp_pair_a);
    uint32_t n = longest_increasing(f->cand, nc, f->tails, f->tails_i, f->prev);
    /* The candidates index ga/gb, which the next gap overwrites: resolve to frame indices first. */
    int32_t pa_prev = a0, pb_prev = b0;
    for (uint32_t k = 0; k < n; k++) {
        int32_t ia = f->ga[f->cand[k].a], jb = f->gb[f->cand[k].b];
        pair_up(f, ia, jb, MOTION_PAIR_FILL);
        if (!push_gap(f, pa_prev, ia, pb_prev, jb)) return 0;
        pa_prev = ia + 1;
        pb_prev = jb + 1;
    }
    return push_gap(f, pa_prev, a1, pb_prev, b1);
}

/* ---- the camera ----------------------------------------------------------------------------- */

static int invert3(const float m[9], float inv[9]) {
    double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    double det = a * A + b * B + c * C;
    if (!(fabs(det) > 1e-20) || !isfinite(det)) return 0;
    double r = 1.0 / det;
    inv[0] = (float)(A * r);
    inv[1] = (float)(-(b * i - c * h) * r);
    inv[2] = (float)((b * f - c * e) * r);
    inv[3] = (float)(B * r);
    inv[4] = (float)((a * i - c * g) * r);
    inv[5] = (float)(-(a * f - c * d) * r);
    inv[6] = (float)(C * r);
    inv[7] = (float)(-(a * h - b * g) * r);
    inv[8] = (float)((a * e - b * d) * r);
    return 1;
}

/* Camera position (-R^-1 t) and the row-normalized rotation of a view matrix in the GE's order:
 * eye[a] = v[a] x + v[3 + a] y + v[6 + a] z + v[9 + a], so R[a][c] = v[3 c + a] and t[a] = v[9 + a]. */
static int camera_pose(const float v[12], float pos[3], float rot[9]) {
    float r[9] = { v[0], v[3], v[6], v[1], v[4], v[7], v[2], v[5], v[8] };
    float t[3] = { v[9], v[10], v[11] }, inv[9];
    if (!invert3(r, inv)) return 0;
    for (int k = 0; k < 3; k++)
        pos[k] = -(inv[k * 3 + 0] * t[0] + inv[k * 3 + 1] * t[1] + inv[k * 3 + 2] * t[2]);
    for (int row = 0; row < 3; row++) {
        double n = sqrt((double)r[row * 3] * r[row * 3] + (double)r[row * 3 + 1] * r[row * 3 + 1] +
                        (double)r[row * 3 + 2] * r[row * 3 + 2]);
        if (!(n > 1e-20)) return 0;
        for (int k = 0; k < 3; k++) rot[row * 3 + k] = (float)(r[row * 3 + k] / n);
    }
    return 1;
}

static float median_of(const float *v, uint32_t n) {
    float s[MOTION_CUT_HISTORY];
    memcpy(s, v, n * sizeof(float));
    for (uint32_t i = 1; i < n; i++)
        for (uint32_t j = i; j > 0 && s[j - 1u] > s[j]; j--) {
            float x = s[j];
            s[j] = s[j - 1u];
            s[j - 1u] = x;
        }
    return (n & 1u) ? s[n / 2u] : 0.5f * (s[n / 2u - 1u] + s[n / 2u]);
}

static int all_finite(const float *v, int n) {
    for (int k = 0; k < n; k++)
        if (!isfinite(v[k])) return 0;
    return 1;
}

static void camera_rules(const MotionCamera *ca, const MotionCamera *cb, const MotionMatchParams *p,
                         MotionCutHistory *hist, MotionMatchResult *out) {
    if (!ca->valid && !cb->valid) return;
    if (ca->valid != cb->valid) {
        out->flags |= MOTION_MATCH_CUT_CAMERA;
        return;
    }
    if (!all_finite(ca->view, 12) || !all_finite(cb->view, 12) || !all_finite(ca->proj, 16) ||
        !all_finite(cb->proj, 16)) {
        out->flags |= MOTION_MATCH_CUT_CAMERA;   /* a non-finite camera: nothing to follow */
        return;
    }
    float pa[3], pb[3], ra[9], rb[9];
    if (!camera_pose(ca->view, pa, ra) || !camera_pose(cb->view, pb, rb)) {
        out->flags |= MOTION_MATCH_CUT_CAMERA;   /* a singular view matrix: no camera to follow */
        return;
    }
    /* rotation angle of Rb Ra^T: acos((trace - 1) / 2) */
    double tr = 0.0;
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) tr += (double)rb[i * 3 + k] * ra[i * 3 + k];
    double c = (tr - 1.0) * 0.5;
    c = c > 1.0 ? 1.0 : (c < -1.0 ? -1.0 : c);
    out->rotate_deg = (float)(acos(c) * (180.0 / 3.14159265358979323846));
    if (out->rotate_deg > p->cut_rotate_deg) out->flags |= MOTION_MATCH_CUT_ROTATE;

    double dx = (double)pb[0] - pa[0], dy = (double)pb[1] - pa[1], dz = (double)pb[2] - pa[2];
    out->translate = (float)sqrt(dx * dx + dy * dy + dz * dz);
    if (hist) {
        /* With a static camera the median is 0 and the rule stays off: starting to move is not a cut. */
        if (hist->n >= MOTION_CUT_HISTORY / 2u) {
            out->translate_median = median_of(hist->step, hist->n);
            if (out->translate_median > 0.0f && out->translate > p->cut_translate_k * out->translate_median)
                out->flags |= MOTION_MATCH_CUT_TRANSLATE;
        }
    }

    float big = 0.0f;
    for (int e = 0; e < 16; e++) {
        float m = fmaxf(fabsf(ca->proj[e]), fabsf(cb->proj[e]));
        big = m > big ? m : big;
    }
    for (int e = 0; e < 16; e++) {
        float m = fmaxf(fabsf(ca->proj[e]), fabsf(cb->proj[e]));
        if (m > big * 1e-3f && fabsf(cb->proj[e] - ca->proj[e]) > p->cut_proj_rel * m) {
            out->flags |= MOTION_MATCH_CUT_PROJ;
            break;
        }
    }
}

/* ---- the matcher ---------------------------------------------------------------------------- */

void motion_match_default_params(MotionMatchParams *p) {
    p->d_max = 68.96f;           /* sqrt(480^2 + 272^2) / 8 */
    p->d_leaf = 32.0f;
    p->cut_rotate_deg = 30.0f;
    p->cut_translate_k = 8.0f;
    p->cut_proj_rel = 0.1f;
    p->cut_min_coverage = 0.5f;
}

static int params_ok(const MotionMatchParams *p) {
    return isfinite(p->d_max) && p->d_max > 0.0f && isfinite(p->d_leaf) && p->d_leaf > 0.0f &&
           isfinite(p->cut_rotate_deg) && p->cut_rotate_deg > 0.0f && isfinite(p->cut_translate_k) &&
           p->cut_translate_k > 0.0f && isfinite(p->cut_proj_rel) && p->cut_proj_rel > 0.0f &&
           p->cut_min_coverage >= 0.0f && p->cut_min_coverage <= 1.0f;   /* also false for NaN */
}

/* A caller-owned history: its counters in range and every recorded step finite and not negative. */
static int hist_ok(const MotionCutHistory *h) {
    if (h->n > MOTION_CUT_HISTORY || h->next >= MOTION_CUT_HISTORY) return 0;
    for (uint32_t k = 0; k < h->n; k++)
        if (!(h->step[k] >= 0.0f) || !isfinite(h->step[k])) return 0;
    return 1;
}

static void fail(int32_t *pair_a, uint8_t *why, uint32_t nb, MotionMatchResult *out) {
    if (pair_a)
        for (uint32_t j = 0; j < nb; j++) pair_a[j] = -1;
    if (why) memset(why, MOTION_PAIR_NONE, nb);
    if (out) {
        memset(out, 0, sizeof(*out));
        out->flags = MOTION_MATCH_FAILED;
        out->unmatched = nb;
    }
}

int motion_match(const MotionMatchFrame *fa, const MotionMatchFrame *fb, const MotionMatchParams *p,
                 MotionCutHistory *hist, int32_t *pair_a, uint8_t *why, MotionMatchResult *out) {
    uint32_t nb = fb ? fb->nprims : 0u;
    if (!fa || !fb || !p || !pair_a || !out || (fa->nprims && !fa->prims) || (nb && !fb->prims) ||
        fa->nprims > MOTION_MATCH_MAX_PRIMS || nb > MOTION_MATCH_MAX_PRIMS || !params_ok(p) ||
        (hist && !hist_ok(hist))) {
        /* The output buffers are sized by B's count; past the cap that count is not trusted. */
        fail(pair_a, why, nb <= MOTION_MATCH_MAX_PRIMS ? nb : 0u, out);
        return -1;
    }
    uint32_t na = fa->nprims;
    memset(out, 0, sizeof(*out));
    for (uint32_t j = 0; j < nb; j++) pair_a[j] = -1;
    if (why) memset(why, MOTION_PAIR_NONE, nb);
    const MotionPrim *A = fa->prims, *B = fb->prims;
    for (uint32_t j = 0; j < nb; j++)
        if (!B[j].through) out->xform_verts += B[j].count;
    if (na == 0) {
        out->flags |= MOTION_MATCH_NO_PREVIOUS;
        out->unmatched = nb;
        return 0;
    }

    size_t n = na > nb ? na : nb;
    HashIdx *ka = malloc(na * sizeof(HashIdx)), *kb = malloc(nb * sizeof(HashIdx) + 1u);
    int32_t *uniq = malloc(nb * sizeof(int32_t) + 1u);
    Fill f;
    memset(&f, 0, sizeof(f));
    f.a = A;
    f.b = B;
    f.pair_a = pair_a;
    f.why = malloc(nb + 1u);            /* reasons are always kept; copied out when asked for */
    f.used_a = calloc(na, 1);
    f.dl2 = p->d_leaf * p->d_leaf;
    f.ga = malloc(n * sizeof(int32_t));
    f.gb = malloc(n * sizeof(int32_t));
    f.ha = malloc(n * sizeof(HashIdx));
    f.hb = malloc(n * sizeof(HashIdx));
    f.cand = malloc(n * sizeof(Pair));
    f.tails = malloc(n * sizeof(int32_t));
    f.prev = malloc(n * sizeof(int32_t));
    f.tails_i = malloc(n * sizeof(uint32_t));
    f.cap_stack = (uint32_t)(na + nb + 2u);
    f.stack = malloc((size_t)f.cap_stack * sizeof(*f.stack));
    f.lcs = malloc((LEAF_LCS_MAX + 1u) * (LEAF_LCS_MAX + 1u) * sizeof(uint16_t));
    int ok = ka && kb && uniq && f.why && f.used_a && f.ga && f.gb && f.ha && f.hb && f.cand && f.tails && f.prev &&
             f.tails_i && f.stack && f.lcs;

    if (ok) {
        memset(f.why, MOTION_PAIR_NONE, nb);
        /* 2. anchors: keys unique in both frames, longest run in order */
        for (uint32_t i = 0; i < na; i++) ka[i] = (HashIdx){ key_hash(&A[i]), i };
        for (uint32_t j = 0; j < nb; j++) kb[j] = (HashIdx){ key_hash(&B[j]), j };
        qsort(ka, na, sizeof(HashIdx), cmp_hash_idx);
        qsort(kb, nb, sizeof(HashIdx), cmp_hash_idx);
        uint32_t nc = 0;
        for (uint32_t j = 0; j < nb; j++) {
            int32_t first_b, first_a;
            uniq[j] = -1;
            uint64_t h = key_hash(&B[j]);
            if (count_exact(kb, nb, h, B, &B[j], key_eq, &first_b) != 1u) continue;
            if (count_exact(ka, na, h, A, &B[j], key_eq, &first_a) != 1u) continue;
            if ((B[j].flags | A[first_a].flags) & MOTION_PRIM_INLINE) continue;   /* F13: never anchors */
            uniq[j] = first_a;
            f.cand[nc++] = (Pair){ first_a, (int32_t)j };
        }
        qsort(f.cand, nc, sizeof(Pair), cmp_pair_a);
        uint32_t nanc = longest_increasing(f.cand, nc, f.tails, f.tails_i, f.prev);
        Pair *anc = malloc((nanc + 1u) * sizeof(Pair));
        ok = anc != NULL;
        if (ok) {
            memcpy(anc, f.cand, nanc * sizeof(Pair));
            for (uint32_t k = 0; k < nanc; k++) pair_up(&f, anc[k].a, anc[k].b, MOTION_PAIR_ANCHOR);
            /* 4. reorder: unique keys outside the ordered run, partner free */
            for (uint32_t j = 0; j < nb; j++)
                if (uniq[j] >= 0 && pair_a[j] < 0 && !f.used_a[uniq[j]]) pair_up(&f, uniq[j], (int32_t)j, MOTION_PAIR_REORDER);
            /* 3. fill the gaps between anchors */
            int32_t a_prev = 0, b_prev = 0;
            for (uint32_t k = 0; k <= nanc && ok; k++) {
                int32_t a_end = k < nanc ? anc[k].a : (int32_t)na, b_end = k < nanc ? anc[k].b : (int32_t)nb;
                ok = push_gap(&f, a_prev, a_end, b_prev, b_end);
                a_prev = a_end + 1;
                b_prev = b_end + 1;
                while (ok && f.nstack) {
                    f.nstack--;
                    int32_t g[4];
                    memcpy(g, f.stack[f.nstack], sizeof(g));
                    ok = fill_gap(&f, g[0], g[1], g[2], g[3]);
                }
            }
            free(anc);
        }
    }

    uint8_t *w = f.why;
    if (ok) {
        /* 5. plausibility */
        float d2 = p->d_max * p->d_max;
        for (uint32_t j = 0; j < nb; j++) {
            int32_t i = pair_a[j];
            if (i < 0) continue;
            if (!(centre_dist2(&A[i], &B[j]) <= d2)) {   /* also drops a non-finite box */
                pair_a[j] = -1;
                f.used_a[i] = 0;
                w[j] = MOTION_PAIR_TELEPORT;
                out->teleports++;
            }
        }
        for (uint32_t j = 0; j < nb; j++) {
            if (pair_a[j] < 0) {
                if (w[j] != MOTION_PAIR_TELEPORT) out->unmatched++;
                continue;
            }
            if (!B[j].through) out->xform_verts_matched += B[j].count;
            if (w[j] == MOTION_PAIR_ANCHOR) out->anchors++;
            else if (w[j] == MOTION_PAIR_FILL) out->fills++;
            else if (w[j] == MOTION_PAIR_REORDER) out->reorders++;
        }
        if (why) memcpy(why, w, nb);
        /* 6. cut */
        camera_rules(&fa->camera, &fb->camera, p, hist, out);
        if (out->xform_verts &&
            (double)out->xform_verts_matched < (double)p->cut_min_coverage * (double)out->xform_verts)
            out->flags |= MOTION_MATCH_CUT_COVERAGE;
        if (hist && fa->camera.valid && fb->camera.valid && !(out->flags & MOTION_MATCH_CUT) &&
            isfinite(out->translate)) {
            hist->step[hist->next] = out->translate;
            hist->next = (hist->next + 1u) % MOTION_CUT_HISTORY;
            if (hist->n < MOTION_CUT_HISTORY) hist->n++;
        }
    }

    free(ka);
    free(kb);
    free(uniq);
    free(f.used_a);
    free(f.ga);
    free(f.gb);
    free(f.ha);
    free(f.hb);
    free(f.cand);
    free(f.tails);
    free(f.prev);
    free(f.tails_i);
    free(f.stack);
    free(f.lcs);
    free(f.why);
    if (!ok) {
        fail(pair_a, why, nb, out);
        return -1;
    }
    return 0;
}
