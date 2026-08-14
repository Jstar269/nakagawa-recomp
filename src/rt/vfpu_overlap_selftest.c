// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
//
// vfpu_overlap_selftest.c — executable VFPU source/destination aliasing suite.
//
// Proves, by executing the code, that the generated C (codegen.vfpu_effect)
// and sr_vfpu_interp both implement read-before-write semantics for the
// matrix/vector op families when destination registers overlap one or both
// sources (vmmul/vtfm/vmscl/vmmov and adjacent vdot/vhdp/vcrs/vscl/vqmul/
// vcrsp).  The case table and the codegen bodies come from the generated
// header vfpu_overlap_cases.h (tools/vfpu_overlap_fuzz_gen.py), which
// enumerates every legal alias class:
//
//   disjoint / vd==vs / vd==vt / partial same-block / transpose-induced /
//   source-source / all-identical / scalar-in-destination,
//
// across .p/.t/.q where the encoding supports it.
//
// Two invariants are asserted per case:
//   1. DIFFERENTIAL: generated-C execution and sr_vfpu_interp leave the full
//      v[] register file and vfpuCtrl bit-identical, over deterministic
//      clobber-revealing vectors AND randomized finite/exotic states with
//      randomized prefixes.
//   2. MODEL (cases the generator marks model-assertable): both implementations
//      equal an INDEPENDENT read-before-write reference computed from a
//      snapshot of the original register file (a different evaluation
//      structure; only the register decode is shared).
//
// Evidence discipline: this is a host-implementation agreement and
// implementation-stability probe.  It does NOT assert that the PSP produces
// these bits for an overlapping encoding.  Public hardware evidence
// (pspdev/vfpu-docs docs/introduction.md "Register hazards" + the hardware
// reg-test generator gen-regtests.py) classifies every case as:
//   ALLOWED      -- disjoint, or docs-compatible overlap (vmscl/vmmov
//                   identical-matrix with the scalar outside the dest);
//   NO_OVERLAP   -- docs state overlapping encodings give incorrect results
//                   on hardware (vmmul/vtfm/vqmul/vcrsp any input/output
//                   overlap; vmscl/vmmov partial or transposed); the host
//                   snapshot semantics asserted here is a host contract only;
//   UNESTABLED   -- no public hardware evidence (source-source overlap,
//                   scalar-inside-destination, vdot/vhdp/vcrs/vscl overlap);
//                   recorded as hardware cells in fixtures/vfpu_overlap_probe/.
// The differential (codegen == interp) and read-before-write model asserts are
// run for every case regardless of contract: they prove the implementations
// agree with each other and with the snapshot reference.  The contract column
// only governs how the result may be LABELED, never which asserts run.
//
// Run via `make vfpu-overlap-selftest` (wired into hst_manager.ps1 -Action
// Verify and the Linux CI gate).  No game inputs or private data required.
//
// OPTIMIZATION-LEVEL CONTRACT: the differential asserts bit-exact agreement
// (NaN payloads included) between two SEPARATELY compiled shapes -- the
// emitted codegen body and the interpreter loop.  That agreement is
// emission-shape-scoped: at -O0 the accumulate-local body and the loop
// compile to the identical addss sequence (issue #40).  Above -O0 a compiler
// may legally reassociate either shape and select a different NaN payload on
// NaN/Inf lanes (observed NaN-vs-NaN payload/sign splits at -O1..-O3, never a
// finite-lane mismatch), so this harness must be compiled at -O0 -- the
// Makefile default and the CI step both do so.

#define _CRT_SECURE_NO_WARNINGS
/* White-box: include the real runtime + interpreter as translation units so the
 * selftest exercises the production sr_vfpu_interp, sr_vread/sr_vwrite, the
 * transcendental kernels and the guest-memory accessors directly (the same
 * pattern vfpu_interp_selftest.c uses). */
#include "recomp.c"
#include "vfpu_tables.c"
#include "vfpu_interp.c"

#include "vfpu_overlap_cases.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, name)                                                      \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);           \
        }                                                                      \
    } while (0)

/* --- Stubs for runtime symbols recomp.c references (vfpu_interp_selftest.c
 * pattern; the tested path uses only the real code). */
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
uint64_t SDL_GetTicksNS(void) { return 0u; }

/* ------------------------------------------------------------------ */
/* Deterministic clobber-revealing states                             */
/* ------------------------------------------------------------------ */

/* Distinct per-lane values: a source lane that is read after being
 * destination-written must produce a different result from the snapshot
 * model, so ANY premature source clobbering shows up immediately. */
static void fill_pattern(CpuState *s, uint32_t seed) {
    uint32_t x = seed ? seed : 0x9E3779B9u;
    for (int i = 0; i < 128; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        int32_t m = (int32_t)(x % 8192) - 4096;
        s->v[i] = (float)m / 32.0f;
    }
    /* identity prefixes; deterministic phase is the read-before-write model */
    s->vfpuCtrl[0] = 0xe4u;
    s->vfpuCtrl[1] = 0xe4u;
    s->vfpuCtrl[2] = 0u;
    s->vfpuCtrl[3] = 0u;
}

/* A couple of structured matrices that make cross-lane products distinctive. */
static void fill_structured(CpuState *s) {
    for (int i = 0; i < 128; i++) {
        int blk = i >> 4, off = i & 15;
        s->v[i] = (float)(blk * 16 + off + 1) + (float)((blk + off) % 3) * 0.25f;
    }
    s->vfpuCtrl[0] = 0xe4u;
    s->vfpuCtrl[1] = 0xe4u;
    s->vfpuCtrl[2] = 0u;
    s->vfpuCtrl[3] = 0u;
}

static uint32_t s_rng = 0x12345678u;
static uint32_t rng(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static float rand_float(void) {
    switch (rng() % 10) {
    case 0: return 0.0f;
    case 1: return 1.0f;
    case 2: return -1.0f;
    case 3: return 0.5f;
    case 4: {
        uint32_t nan = 0x7F800001;
        float f;
        memcpy(&f, &nan, 4);
        return f;
    }
    case 5: {
        uint32_t inf = 0x7F800000;
        float f;
        memcpy(&f, &inf, 4);
        return f;
    }
    default: {
        int32_t m = (int32_t)(rng() % 4000) - 2000;
        return (float)m / 16.0f;
    }
    }
}

static uint32_t rand_sprefix(void) {
    if (rng() % 2) return 0xe4;
    uint32_t p = 0;
    for (int i = 0; i < 4; i++) p |= (rng() & 3u) << (i * 2);
    p |= (rng() & 0xFu) << 8;
    if (rng() % 4 == 0) p |= (rng() & 0xFu) << 12;
    p |= (rng() & 0xFu) << 16;
    return p;
}

static uint32_t rand_dprefix(void) {
    if (rng() % 2) return 0;
    uint32_t p = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t sat = rng() & 3u;
        if (sat == 2) sat = 0;
        p |= sat << (i * 2);
    }
    p |= (rng() & 0xFu) << 8;
    return p;
}

static void fill_random(CpuState *s, int identity_only) {
    for (int i = 0; i < 128; i++) s->v[i] = rand_float();
    if (identity_only) {
        s->vfpuCtrl[0] = 0xe4u;
        s->vfpuCtrl[1] = 0xe4u;
        s->vfpuCtrl[2] = 0u;
    } else {
        s->vfpuCtrl[0] = rand_sprefix();
        s->vfpuCtrl[1] = rand_sprefix();
        s->vfpuCtrl[2] = rand_dprefix();
    }
    s->vfpuCtrl[3] = rng() & 0x3Fu;
}

/* ------------------------------------------------------------------ */
/* Independent read-before-write reference model                       */
/* ------------------------------------------------------------------ */

/* Computes the op's result from a SNAPSHOT of the original register file,
 * with identity prefixes.  Only the register decode (vreg_idx/mreg_idx) is
 * shared with the implementations; the evaluation structure (snapshot-first,
 * all reads before any write) is independent of both sr_vfpu_interp's code
 * and the emitted C. */
static void model_op(const CpuState *orig, uint32_t w, int family, CpuState *out) {
    *out = *orig;
    float snap[128];
    for (int i = 0; i < 128; i++) snap[i] = orig->v[i];

    int vd = (int)(w & 0x7F), vs = (int)((w >> 8) & 0x7F), vt = (int)((w >> 16) & 0x7F);
    int n = (((w >> 7) & 1) | ((w >> 14) & 2)) + 1;
    int sub = (int)((w >> 23) & 7);

    switch (family) {
    case OVERLAP_FAM_VMMUL: {
        int side = n;
        for (int a = 0; a < side; a++)
            for (int b = 0; b < side; b++) {
                float sum = 0.0f;
                for (int c = 0; c < side; c++)
                    sum += snap[mreg_idx(vs, side, b, c)] * snap[mreg_idx(vt, side, a, c)];
                out->v[mreg_idx(vd, side, a, b)] = sum;
            }
        break;
    }
    case OVERLAP_FAM_VTFM: {
        int ins = sub, side = ins + 1, tn = n < ins + 1 ? n : ins + 1;
        uint8_t ti[4], di[4];
        vreg_idx(vt, side, ti);
        vreg_idx(vd, side, di);
        for (int i = 0; i < side; i++) {
            float sum = 0.0f;
            for (int k = 0; k < tn; k++) sum += snap[mreg_idx(vs, side, i, k)] * snap[ti[k]];
            if (ins >= n) sum += snap[mreg_idx(vs, side, i, ins)];
            out->v[di[i]] = sum;
        }
        break;
    }
    case OVERLAP_FAM_VMSCL: {
        int side = n;
        uint8_t sc[1];
        vreg_idx(vt, 1, sc);
        float scalar = snap[sc[0]];
        for (int i = 0; i < side; i++)
            for (int j = 0; j < side; j++)
                out->v[mreg_idx(vd, side, j, i)] = snap[mreg_idx(vs, side, j, i)] * scalar;
        break;
    }
    case OVERLAP_FAM_VMMOV: {
        int side = n;
        for (int i = 0; i < side; i++)
            for (int j = 0; j < side; j++)
                out->v[mreg_idx(vd, side, j, i)] = snap[mreg_idx(vs, side, j, i)];
        break;
    }
    case OVERLAP_FAM_VDOT: {
        uint8_t si[4], ti[4], dst[1];
        vreg_idx(vs, n, si);
        vreg_idx(vt, n, ti);
        vreg_idx(vd, 1, dst);
        float acc = 0.0f;
        for (int i = 0; i < n; i++) acc += snap[si[i]] * snap[ti[i]];
        out->v[dst[0]] = acc;
        break;
    }
    case OVERLAP_FAM_VHDP: {
        uint8_t si[4], ti[4], dst[1];
        vreg_idx(vs, n, si);
        vreg_idx(vt, n, ti);
        vreg_idx(vd, 1, dst);
        float acc = 0.0f;
        for (int i = 0; i < n - 1; i++) acc += snap[si[i]] * snap[ti[i]];
        acc += 1.0f * snap[ti[n - 1]];
        out->v[dst[0]] = isnan(acc) ? fabsf(acc) : acc;
        break;
    }
    case OVERLAP_FAM_VCRS: {
        static const int ss[4] = {1, 2, 0, 3}, ts[4] = {2, 0, 1, 3};
        uint8_t si[4], ti[4], di[4];
        vreg_idx(vs, 3, si);
        vreg_idx(vt, 3, ti);
        vreg_idx(vd, 3, di);
        for (int i = 0; i < 3; i++) out->v[di[i]] = snap[si[ss[i]]] * snap[ti[ts[i]]];
        break;
    }
    case OVERLAP_FAM_VSCL: {
        uint8_t si[4], di[4], sc[1];
        vreg_idx(vs, n, si);
        vreg_idx(vt, 1, sc);
        vreg_idx(vd, n, di);
        float scalar = snap[sc[0]];
        for (int i = 0; i < n; i++) out->v[di[i]] = snap[si[i]] * scalar;
        break;
    }
    case OVERLAP_FAM_VQMUL: {
        uint8_t si[4], ti[4], di[4];
        vreg_idx(vs, 4, si);
        vreg_idx(vt, 4, ti);
        vreg_idx(vd, 4, di);
        float a[4], b[4];
        for (int i = 0; i < 4; i++) {
            a[i] = snap[si[i]];
            b[i] = snap[ti[i]];
        }
        out->v[di[0]] =  a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
        out->v[di[1]] = -a[0] * b[2] + a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
        out->v[di[2]] =  a[0] * b[1] - a[1] * b[0] + a[2] * b[3] + a[3] * b[2];
        out->v[di[3]] = -a[0] * b[0] - a[1] * b[1] - a[2] * b[2] + a[3] * b[3];
        break;
    }
    case OVERLAP_FAM_VCRSP: {
        uint8_t si[4], ti[4], di[4];
        vreg_idx(vs, 3, si);
        vreg_idx(vt, 3, ti);
        vreg_idx(vd, 3, di);
        float a[4], b[4];
        for (int i = 0; i < 3; i++) {
            a[i] = snap[si[i]];
            b[i] = snap[ti[i]];
        }
        out->v[di[0]] = a[1] * b[2] - a[2] * b[1];
        out->v[di[1]] = a[2] * b[0] - a[0] * b[2];
        out->v[di[2]] = a[0] * b[1] - a[1] * b[0];
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Harness                                                            */
/* ------------------------------------------------------------------ */

static int state_equal(const CpuState *a, const CpuState *b) {
    for (int i = 0; i < 128; i++)
        if (a->vi[i] != b->vi[i]) return 0;
    for (int i = 0; i < 4; i++)
        if (a->vfpuCtrl[i] != b->vfpuCtrl[i]) return 0;
    return 1;
}

static void report_lane_diff(const CpuState *a, const CpuState *b, uint32_t w, int idx) {
    for (int i = 0; i < 128; i++)
        if (a->vi[i] != b->vi[i]) {
            fprintf(stderr,
                    "  case %d op=0x%08x: v[%d] codegen/interp=0x%08x other=0x%08x (%g vs %g)\n",
                    idx, w, i, a->vi[i], b->vi[i], a->v[i], b->v[i]);
            return;
        }
}

int main(int argc, char **argv) {
    int trials = argc > 1 ? atoi(argv[1]) : 64;
    if (getenv("OVERLAP_TRIALS")) trials = atoi(getenv("OVERLAP_TRIALS"));
    if (argc > 2) s_rng = (uint32_t)strtoul(argv[2], NULL, 0);
    if (getenv("OVERLAP_SEED")) s_rng = (uint32_t)strtoul(getenv("OVERLAP_SEED"), NULL, 0);

    int bad_cases = 0, model_checked = 0, differential_trials = 0;
    int n_allowed = 0, n_no_overlap = 0, n_unestablished = 0;

    for (int c = 0; c < VFPU_OVERLAP_NCASES; c++) {
        uint32_t w = vfpu_overlap_cases[c].w;
        int family = vfpu_overlap_cases[c].family;
        int assert_model = vfpu_overlap_cases[c].assert_model;
        int contract = vfpu_overlap_cases[c].contract;
        int identity_only = (family == OVERLAP_FAM_VQMUL || family == OVERLAP_FAM_VCRSP);
        int case_bad = 0;
        switch (contract) {
        case OVERLAP_CT_ALLOWED: n_allowed++; break;
        case OVERLAP_CT_NO_OVERLAP: n_no_overlap++; break;
        default: n_unestablished++; break;
        }

        /* --- Phase A: deterministic clobber-revealing vectors, identity
         * prefixes; codegen == interp == model (model where assertable). */
        const CpuState *init_states[2];
        CpuState det0, det1;
        fill_pattern(&det0, 0x5EED1234u + (uint32_t)c);
        fill_structured(&det1);
        init_states[0] = &det0;
        init_states[1] = &det1;

        for (int st = 0; st < 2 && !case_bad; st++) {
            CpuState s0 = *init_states[st];
            CpuState s1 = s0, s2 = s0;
            int kind = sr_vfpu_interp(&s2, w);
            if (kind == SR_VFPU_OTHER) {
                fprintf(stderr, "UNCOVERED case %d op=0x%08x: interp has no oracle for it\n", c, w);
                case_bad = 1;
                break;
            }
            fuzz_overlap_run_codegen(&s1, c);
            differential_trials++;
            if (!state_equal(&s1, &s2)) {
                report_lane_diff(&s1, &s2, w, c);
                CHECK(0, "differential (deterministic state): codegen == interp");
                case_bad = 1;
                break;
            }
            if (assert_model) {
                CpuState m;
                model_op(&s0, w, family, &m);
                model_checked++;
                if (!state_equal(&s1, &m)) {
                    report_lane_diff(&s1, &m, w, c);
                    CHECK(0, "model (deterministic state): codegen == read-before-write reference");
                    case_bad = 1;
                    break;
                }
            }
        }
        if (case_bad) {
            bad_cases++;
            continue;
        }

        /* --- Phase B: randomized states + randomized prefixes
         * (identity prefixes for the identity-only families). */
        for (int t = 0; t < trials && !case_bad; t++) {
            CpuState s0;
            memset(&s0, 0, sizeof(s0));
            fill_random(&s0, identity_only);
            CpuState s1 = s0, s2 = s0;
            int kind = sr_vfpu_interp(&s2, w);
            if (kind == SR_VFPU_OTHER) continue;
            fuzz_overlap_run_codegen(&s1, c);
            differential_trials++;
            if (!state_equal(&s1, &s2)) {
                report_lane_diff(&s1, &s2, w, c);
                CHECK(0, "differential (randomized state): codegen == interp");
                case_bad = 1;
                break;
            }
        }
        if (case_bad) bad_cases++;
    }

    if (bad_cases == 0) {
        printf("vfpu_overlap_selftest: %d cases x differential, %d model comparisons, %d trials OK\n",
               VFPU_OVERLAP_NCASES, model_checked, differential_trials);
        printf("  hardware contract: %d ALLOWED, %d NO_OVERLAP (host snapshot only), "
               "%d UNESTABLED (hardware cells in fixtures/vfpu_overlap_probe/)\n",
               n_allowed, n_no_overlap, n_unestablished);
    } else {
        fprintf(stderr, "vfpu_overlap_selftest: %d/%d cases FAILED\n", bad_cases, VFPU_OVERLAP_NCASES);
    }
    bad_cases |= g_failures != 0;
    return bad_cases ? 1 : 0;
}
