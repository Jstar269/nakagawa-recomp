// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
//
// vfpu_interp_selftest.c — executable regression suite for issue #184.
//
// Drives the REAL sr_vfpu_interp (the production VFPU fallback decoder that
// codegen.py delegates lvl/lvr/svl/svr and misaligned lv.q/sv.q to) against a
// real guest arena. It proves, by executing the code rather than by reading
// its shape:
//
//   * quad loads/stores are all-or-nothing: a span that is not entirely
//     guest-readable/writable rejects the whole op BEFORE any destination lane
//     or guest word is committed (destination and guest state verified
//     unchanged), including exact-end, one-lane-short, first-byte-invalid and
//     wrapped-address cases;
//   * the left/right merge forms preflight their exact sub-span (which always
//     lies inside the aligned 16-byte window containing the address); the last
//     fully in-range window is accepted for each form, the first out-of-range
//     window is rejected with zero state change;
//   * rejected ops do not consume S/T/D prefix state (full CpuState memcmp);
//   * vcrs rejects non-triple widths before any source read (full-state
//     memcmp, mirroring the fuzzer's guard check) and computes the triple form;
//   * the vrot overlap quirk only inspects ACTIVE destination lanes — a scalar
//     source register 0 must not match a zero-filled inactive pair lane, while
//     an active source lane still triggers the cosine-recompute quirk;
//   * the shared VASIN helper matches the real-PSP measured domain words,
//     including signed invalid-NaN payloads just outside +/-1;
//   * vscmp.s/vsge.s/vslt.s return the 45 words a PSP-3000 measured
//     (PSP-VFPU-CMP-001), in the generated code and the interpreter alike, and
//     vscmp at every width follows the PSP-HLE-ISSUES-001 cells (denormals
//     flushed, NaNs ordered by bit pattern);
//   * vrndi.s steps the random unit by the PSP-HLE-ISSUES-001 rule in the
//     generated code and the interpreter alike, and the vrnd forms and prefixes
//     without a recorded rule fail closed;
//   * vh2f.s/.p, vuc2i.s, vsocp.s/.p, vwbn.s and vrndf1.s/vrndf2.s return the
//     PSP-VFPU-FORMS-001 words in both lanes, and their unmeasured widths and
//     prefixes fail closed;
//   * valid aligned quad round-trips still work.
//
// The final check count is printed by the executable. No game inputs or private
// data required. Run via `make vfpu-interp-selftest` (also wired into
// nk_manager.ps1 -Action Verify and the Linux native CI gate, where the
// ASan/UBSan build runs as the uninitialized-read/overflow gate).

#define _CRT_SECURE_NO_WARNINGS
/* White-box: include the real runtime + interpreter as translation units so the
 * selftest exercises the production sr_vfpu_interp, sr_vread/sr_vwrite, the
 * transcendental kernels and the guest-memory accessors directly (the same
 * pattern src/rt/heap_selftest.c uses for recomp.c). */
#include "recomp.c"
#include "vfpu_tables.c"
#include "vfpu_interp.c"
#include "vfpu_overlap_diff_cases.h"

/* No GE renderer is linked by this VFPU-only harness. */
volatile int g_sr_vram_read_pending;
void sr_ge_vram_read_prepare(uint32_t addr, uint32_t width) { (void)addr; (void)width; }
void sr_ge_vram_write_prepare(uint32_t addr, uint32_t width) { (void)addr; (void)width; }
void sr_ge_vram_write_commit(uint32_t addr, uint32_t width) { (void)addr; (void)width; }

#include <fenv.h>
#include <stdio.h>
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
#define CHECKB(badvar, cond, name)                                             \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            (badvar) = 1;                                                      \
            g_failures++;                                                      \
            fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);           \
        }                                                                      \
    } while (0)

/* --- Stubs for runtime symbols recomp.c references (mirrors heap_selftest.c).
 * These back the scheduler/driver/HLE plumbing that the interp path never
 * invokes on the cases below; the tested path uses only the real code. */
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

#define VFPU_OP(op) ((uint32_t)(op) << 26)

static uint32_t lvq(int rs, int vt, uint32_t imm) {
    return VFPU_OP(0x36) | ((uint32_t)rs << 21) | ((uint32_t)vt << 16) | (imm & 0xFFFCu);
}
static uint32_t svq(int rs, int vt, uint32_t imm) {
    return VFPU_OP(0x3e) | ((uint32_t)rs << 21) | ((uint32_t)vt << 16) | (imm & 0xFFFCu);
}
static uint32_t lvlq(int rs, int vt, uint32_t imm) {
    return VFPU_OP(0x35) | ((uint32_t)rs << 21) | ((uint32_t)vt << 16) | (imm & 0xFFFCu);
}
static uint32_t svlq(int rs, int vt, uint32_t imm) {
    return VFPU_OP(0x3d) | ((uint32_t)rs << 21) | ((uint32_t)vt << 16) | (imm & 0xFFFCu);
}

/* Arena end: the guest arena covers phys [0, 0x0c000000). */
#define ARENA_END 0x0c000000u

/* Fresh state with deliberately NON-identity prefixes so prefix consumption
 * (or the absence of it) is detectable on rejected ops. Compute-path tests
 * override vfpuCtrl[2] to 0 (identity D prefix) before asserting values. */
static void setup_state(CpuState *s) {
    memset(s, 0, sizeof(*s));
    s->vfpuCtrl[0] = 0x12345678u;
    s->vfpuCtrl[1] = 0x23456789u;
    s->vfpuCtrl[2] = 0x3456789au;
}

static void setup_identity_compute(CpuState *s) {
    memset(s, 0, sizeof(*s));
    s->vfpuCtrl[0] = 0xe4u;  /* identity S prefix */
    s->vfpuCtrl[1] = 0xe4u;  /* identity T prefix */
    s->vfpuCtrl[2] = 0u;     /* identity D prefix */
}

static void fill_vi(CpuState *s, uint32_t base) {
    for (int i = 0; i < 4; i++) s->vi[base + (uint32_t)i] = 0xA5A5A500u + (uint32_t)i;
}

static int vi_unchanged(const CpuState *s, uint32_t base, uint32_t value0) {
    for (int i = 0; i < 4; i++)
        if (s->vi[base + (uint32_t)i] != value0 + (uint32_t)i) return 0;
    return 1;
}

static uint32_t cop2_transfer(int sub, int rt, int imm) {
    return (0x12u << 26) | ((uint32_t)sub << 21) |
           ((uint32_t)rt << 16) | (uint32_t)imm;
}

static int check_cop2_control_transfers(void) {
    CpuState s;
    int bad = 0;

    /* 128..143 are the complete vfpuCtrl[0..15] register window. Exercise
     * both directions through the production interpreter, including the
     * upper twelve indices that previously fell through the guard. */
    for (int i = 0; i < 16; i++) {
        const uint32_t value = 0xA5000000u + (uint32_t)i;
        setup_state(&s);
        s.vfpuCtrl[i] = value;
        CHECK(sr_vfpu_interp(&s, cop2_transfer(3, 5, 128 + i)) == SR_VFPU_COMPUTE,
              "mfvc accepts every VFPU control register");
        CHECK(s.r[5] == value, "mfvc reads the selected VFPU control register");

        setup_state(&s);
        s.r[5] = value;
        CHECK(sr_vfpu_interp(&s, cop2_transfer(7, 5, 128 + i)) == SR_VFPU_COMPUTE,
              "mtvc accepts every VFPU control register");
        CHECK(s.vfpuCtrl[i] == value, "mtvc writes the selected VFPU control register");
    }

    setup_state(&s);
    const CpuState before = s;
    CHECK(sr_vfpu_interp(&s, cop2_transfer(3, 5, 144)) == SR_VFPU_OTHER,
          "mfvc rejects the first control register outside the window");
    CHECK(memcmp(&s, &before, sizeof(s)) == 0,
          "invalid mfvc leaves the complete CpuState unchanged");
    bad |= memcmp(&s, &before, sizeof(s)) != 0;
    return bad;
}

static int check_quad_memops(void) {
    CpuState s;
    int bad = 0;

    /* 1. Valid aligned lv.q/sv.q round-trip inside RAM. */
    sr_mem_init();
    setup_state(&s);
    s.r[4] = 0x08001000u;
    for (int i = 0; i < 4; i++)
        MEM_W32(0x08001000u + (uint32_t)i * 4u, 0x11111111u * (uint32_t)(i + 1));
    fill_vi(&s, 0);
    CHECK(sr_vfpu_interp(&s, lvq(4, 0, 0)) == SR_VFPU_COMPUTE,
          "lv.q returns COMPUTE for an in-range span");
    CHECK(s.vi[0] == 0x11111111u && s.vi[1] == 0x22222222u &&
          s.vi[2] == 0x33333333u && s.vi[3] == 0x44444444u,
          "lv.q loads all four lanes");

    setup_state(&s);
    s.r[4] = 0x08002000u;
    for (int i = 0; i < 4; i++) s.vi[i] = 0x0bad0000u + (uint32_t)i;
    CHECK(sr_vfpu_interp(&s, svq(4, 0, 0)) == SR_VFPU_STATE,
          "sv.q returns STATE for an in-range span");
    CHECK(MEM_R32(0x08002000u) == 0x0bad0000u && MEM_R32(0x08002004u) == 0x0bad0001u &&
          MEM_R32(0x08002008u) == 0x0bad0002u && MEM_R32(0x0800200cu) == 0x0bad0003u,
          "sv.q stores all four lanes");

    /* 2. Span exactly ending at the arena boundary is legal. */
    setup_state(&s);
    s.r[4] = ARENA_END - 16u;  /* [ARENA_END-16, ARENA_END) fits exactly */
    fill_vi(&s, 0);
    CHECK(sr_vfpu_interp(&s, lvq(4, 0, 0)) == SR_VFPU_COMPUTE,
          "lv.q span exactly ending at arena end is accepted");

    /* 3. Final lane invalid: [ARENA_END-12, ARENA_END+4) crosses by one word.
     * The pre-#184 code committed lanes 0-2 before faulting on lane 3. */
    {
        setup_state(&s);
        s.r[4] = ARENA_END - 12u;
        fill_vi(&s, 0);
        const CpuState before = s;
        CHECK(sr_vfpu_interp(&s, lvq(4, 0, 0)) == SR_VFPU_OTHER,
              "lv.q straddling the arena end is rejected");
        CHECK(vi_unchanged(&s, 0, 0xA5A5A500u),
              "lv.q rejection leaves every destination lane unchanged");
        CHECK(memcmp(&s, &before, sizeof(s)) == 0,
              "lv.q rejection leaves the whole CpuState (incl. prefixes) unchanged");
        bad |= !vi_unchanged(&s, 0, 0xA5A5A500u);
    }

    /* 4. One-lane-short: [ARENA_END-8, ARENA_END+8) — half the span valid. */
    setup_state(&s);
    s.r[4] = ARENA_END - 8u;
    fill_vi(&s, 0);
    CHECK(sr_vfpu_interp(&s, lvq(4, 0, 0)) == SR_VFPU_OTHER,
          "lv.q with only half the span in range is rejected");
    CHECK(vi_unchanged(&s, 0, 0xA5A5A500u), "half-in-range lv.q leaves destination unchanged");

    /* 5. First byte invalid: base exactly at the arena end. */
    setup_state(&s);
    s.r[4] = ARENA_END;
    fill_vi(&s, 0);
    CHECK(sr_vfpu_interp(&s, lvq(4, 0, 0)) == SR_VFPU_OTHER,
          "lv.q starting at the arena end is rejected");

    /* 6. Wrapped 32-bit address: base near UINT32_MAX with a negative offset
     * lands at 0xFFFF7FFF (phys 0x1FFF7FFF, outside the arena). */
    setup_state(&s);
    s.r[4] = 0xFFFFFFFFu;
    fill_vi(&s, 0);
    CHECK(sr_vfpu_interp(&s, lvq(4, 0, 0x8000u)) == SR_VFPU_OTHER,
          "lv.q with a wrapped OOR address is rejected");
    CHECK(vi_unchanged(&s, 0, 0xA5A5A500u), "wrapped lv.q leaves destination unchanged");

    /* 6b. Alias boundary: SR_PHYS(a) = a & 0x1FFFFFFF, so 0x2Bxxxxxx aliases
     * to phys 0x0Bxxxxxx (the same guest RAM). A window exactly ending at the
     * arena end through the alias is legal (same accepted set as scalar
     * accessors); one byte further aliases past phys 0x0C000000 and must
     * reject all-or-nothing. */
    setup_state(&s);
    s.r[4] = 0x2BFFFFF0u;  /* aliases to 0x0BFFFFF0, window [.., 0x0C000000) */
    fill_vi(&s, 0);
    for (int i = 0; i < 4; i++) MEM_W32(0x0BFFFFF0u + (uint32_t)i * 4u, 0x51515151u + (uint32_t)i);
    CHECK(sr_vfpu_interp(&s, lvq(4, 0, 0)) == SR_VFPU_COMPUTE &&
          s.vi[0] == 0x51515151u && s.vi[3] == 0x51515154u,
          "lv.q through the phys alias at the arena end is accepted");
    setup_state(&s);
    s.r[4] = 0x2BFFFFF4u;  /* window [0x0BFFFFF4, 0x0C000004): crosses the end */
    fill_vi(&s, 0);
    CHECK(sr_vfpu_interp(&s, lvq(4, 0, 0)) == SR_VFPU_OTHER && vi_unchanged(&s, 0, 0xA5A5A500u),
          "lv.q through the phys alias crossing the arena end is rejected");

    /* 7. sv.q rejection must not mutate ANY guest word (pre-#184 wrote the
     * first three words then faulted on the fourth). */
    setup_state(&s);
    s.r[4] = ARENA_END - 12u;  /* crossing span */
    for (int i = 0; i < 4; i++) s.vi[i] = 0x0bad0000u + (uint32_t)i;
    for (int i = 0; i < 4; i++) MEM_W32(ARENA_END - 12u + (uint32_t)i * 4u, 0xCAFE0000u + (uint32_t)i);
    CHECK(sr_vfpu_interp(&s, svq(4, 0, 0)) == SR_VFPU_OTHER,
          "sv.q straddling the arena end is rejected");
    CHECK(MEM_R32(ARENA_END - 12u) == 0xCAFE0000u && MEM_R32(ARENA_END - 8u) == 0xCAFE0001u &&
          MEM_R32(ARENA_END - 4u) == 0xCAFE0002u,
          "sv.q rejection leaves every earlier guest word unchanged");

    /* 8. Left/right merge forms preflight their exact sub-span. The sub-span
     * always lies inside the aligned 16-byte window containing addr (the left
     * form starts at the window base, the right form ends at the window top),
     * and the arena end is itself 16-byte aligned, so a window is either fully
     * in range or fully out: there is no partially-OOR window at this boundary.
     * The tests therefore pin the exact boundary semantics: the last fully
     * in-range window (and each of its sub-forms) is accepted, the first
     * out-of-range window is rejected with zero state change. */
    setup_state(&s);
    s.r[4] = ARENA_END - 4u;  /* window [ARENA_END-16, ARENA_END): fully in range */
    fill_vi(&s, 0);
    MEM_W32(ARENA_END - 4u, 0x77777777u);
    CHECK(sr_vfpu_interp(&s, lvlq(4, 0, 0)) == SR_VFPU_COMPUTE && s.vi[3] == 0x77777777u,
          "lvl.q with a single in-range word is accepted");
    /* lvr.q (right variant, bit 1 of the low 16 selects it): word offset 3 ->
     * sub-span [ARENA_END-4, ARENA_END), one word, also fully in range. */
    setup_state(&s);
    s.r[4] = ARENA_END - 4u;
    fill_vi(&s, 0);
    CHECK(sr_vfpu_interp(&s, lvlq(4, 0, 0) | 2u) == SR_VFPU_COMPUTE && s.vi[0] == 0x77777777u,
          "lvr.q with a single in-range word is accepted");
    /* svl.q (left store) over the full window stores all four words. */
    setup_state(&s);
    s.r[4] = ARENA_END - 4u;
    for (int i = 0; i < 4; i++) s.vi[i] = 0x0bad0000u + (uint32_t)i;
    /* svl.q at word offset 3 stores vi[3] at addr and vi[0] at addr-12. */
    CHECK(sr_vfpu_interp(&s, svlq(4, 0, 0)) == SR_VFPU_STATE &&
          MEM_R32(ARENA_END - 16u) == 0x0bad0000u && MEM_R32(ARENA_END - 4u) == 0x0bad0003u,
          "svl.q stores the full in-range window");
    /* First out-of-range window: base exactly at the arena end rejects the
     * left/right forms too, with no destination or prefix change. */
    {
        setup_state(&s);
        s.r[4] = ARENA_END;
        fill_vi(&s, 0);
        const CpuState before = s;
        CHECK(sr_vfpu_interp(&s, lvlq(4, 0, 0) | 2u) == SR_VFPU_OTHER && vi_unchanged(&s, 0, 0xA5A5A500u),
              "lvr.q at the first OOR window is rejected without destination change");
        CHECKB(bad, memcmp(&s, &before, sizeof(s)) == 0,
               "lvr.q rejection leaves the whole CpuState (incl. prefixes) unchanged");
    }
    /* svl.q at the first OOR window leaves guest memory unchanged. */
    setup_state(&s);
    s.r[4] = ARENA_END;
    for (int i = 0; i < 4; i++) s.vi[i] = 0x0bad0000u + (uint32_t)i;
    CHECK(sr_vfpu_interp(&s, svlq(4, 0, 0)) == SR_VFPU_OTHER,
          "svl.q at the first OOR window is rejected");

    return bad;
}

static int check_vcrs_widths(void) {
    CpuState s;
    int bad = 0;
    const uint32_t base = (0x19u << 26) | (5u << 23) | (1u << 16) | (2u << 8) | 3u;
    /* vs=2, vt=1, vd=3; vsize(w)=(((w>>7)&1)|((w>>14)&2))+1, so the width
     * encodings are .s=none, .p=bit7, .t=bit15, .q=bits15|7. All reserved
     * widths (1, 2, 4 lanes) must be rejected; only .t (3 lanes) is legal. */
    const uint32_t widths[] = {0u, 1u << 7, (1u << 15) | (1u << 7)};
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        setup_state(&s);
        const CpuState before = s;
        int rc = sr_vfpu_interp(&s, base | widths[i]);
        if (rc != SR_VFPU_OTHER || memcmp(&s, &before, sizeof(s)) != 0) bad = 1;
    }
    CHECK(!bad, "vcrs rejects .s/.p/.q widths with zero state change (no prefix consumption)");

    /* Legal triple form: physical lanes for vs=2 (n=3) are {8,9,10}, vt=1 are
     * {4,5,6}, vd=3 are {12,13,14}. ss={1,2,0,3}, ts={2,0,1,3}:
     * d[0]=a[1]*b[2], d[1]=a[2]*b[0], d[2]=a[0]*b[1]. */
    setup_identity_compute(&s);
    s.v[8] = 2.0f; s.v[9] = 3.0f; s.v[10] = 5.0f;   /* a = {2,3,5} */
    s.v[4] = 7.0f; s.v[5] = 11.0f; s.v[6] = 13.0f;  /* b = {7,11,13} */
    s.vi[12] = 0; s.vi[13] = 0; s.vi[14] = 0;
    int rc = sr_vfpu_interp(&s, base | (2u << 14));  /* .t width (bit 15) */
    CHECK(rc == SR_VFPU_COMPUTE, "vcrs.t is a compute op");
    CHECK(s.v[12] == 3.0f * 13.0f && s.v[13] == 5.0f * 7.0f && s.v[14] == 2.0f * 11.0f,
          "vcrs.t lane permutation matches the triple-vector contract");
    return bad;
}

static int check_vrot_overlap(void) {
    CpuState s;
    int bad = 0;
    /* vrot word: op 0x3c, sub 7 (bits 25:23), idx 29 (bits 25:21). */
    const uint32_t vrot = (0x3cu << 26) | (7u << 23) | (29u << 21);

    /* Inactive-lane regression: vrot.p, vs=0 (scalar angle at physical v[0]),
     * vd=1 (dest physical lanes {4,5}; register numbers {1,33}). The overlap
     * scan's zero-filled inactive dn entries are register 0, so scanning all
     * four would falsely match vs==0 and recompute the cosine from an inactive
     * zero lane. The fixed scan only inspects the n active lanes.
     * imm=1: sl=0 (sine lane), cl=1 (cosine lane). */
    setup_identity_compute(&s);
    s.v[0] = 0.5f;
    s.vi[4] = 0xDEAD0000u;
    s.vi[5] = 0xDEAD0001u;
    uint32_t w = vrot | (1u << 7) | (1u << 16) | (0u << 8) | 1u;  /* .p width */
    CHECK(sr_vfpu_interp(&s, w) == SR_VFPU_COMPUTE, "vrot.p computes");
    /* vd=1 with n=2 -> physical dest lanes {4,5}; d[0]=sine, d[1]=cosine. */
    CHECK(s.v[4] == sr_vfpu_sin(0.5f) && s.v[5] == sr_vfpu_cos(0.5f),
          "vrot.p does not recompute cosine from an inactive zero lane");
    if (s.v[5] != sr_vfpu_cos(0.5f)) bad = 1;

    /* Positive control: vs=1 (register 1 = physical v[4], an ACTIVE dest
     * lane). The cosine is recomputed from the written sine lane, giving
     * cos(sin(angle)) — the PPSSPP overlap quirk. */
    setup_identity_compute(&s);
    s.v[4] = 0.5f;  /* angle lives in an ACTIVE dest lane (v[4] == vd lane 0) */
    w = vrot | (1u << 7) | (1u << 16) | (1u << 8) | 1u;  /* .p width */
    CHECK(sr_vfpu_interp(&s, w) == SR_VFPU_COMPUTE &&
          s.v[5] == sr_vfpu_cos(sr_vfpu_sin(0.5f)) && s.v[4] == sr_vfpu_sin(0.5f),
          "vrot.p recomputes cosine from the written sine lane when vs is an active dest lane");
    return bad;
}

static int check_vhdp_vmscl_overlap(void) {
    CpuState s;
    int bad = 0;

    /* vhdp.t (op 0x19 sub 4): destination overlaps source lanes.
     * vd=0x20 (physical v[1] for scalar result), vs=0x20 (v[0],v[4],v[8]), vt=0x00 (v[0],v[1],v[2]).
     * The scalar term is 1.0 * t[2] = v[2]. */
    setup_identity_compute(&s);
    s.v[0] = 2.0f;
    s.v[4] = 3.0f;
    s.v[8] = 5.0f;
    s.v[1] = 7.0f;
    s.v[2] = 11.0f;
    uint32_t vhdp = (0x19u << 26) | (4u << 23) | (0u << 16) | (0x20u << 8) | (1u << 15) | 0x20u;
    CHECK(sr_vfpu_interp(&s, vhdp) == SR_VFPU_COMPUTE, "vhdp.t overlap computes");
    CHECK(s.v[1] == 2.0f * 2.0f + 3.0f * 7.0f + 1.0f * 11.0f,
          "vhdp.t overlap accumulation matches dot-product contract");
    if (s.v[1] != 2.0f * 2.0f + 3.0f * 7.0f + 1.0f * 11.0f) bad = 1;

    /* vmscl.t (op 0x3C sub 4): scalar inside destination matrix.
     * vd=0x20 (v[0..10]), vs=0x20 (v[0..10]), vt=0x00 (scalar at v[0]).
     * The scalar is read once before the early-row loop, but the last row
     * re-reads it via sr_vread from the live v[] array, so if the scalar
     * register lies inside the destination it sees the clobbered value. */
    setup_identity_compute(&s);
    s.v[0] = 3.0f;  /* scalar */
    s.v[1] = 2.0f;  /* source row 0 col 1 */
    s.v[4] = 5.0f;  /* source row 1 col 0 */
    s.v[9] = 7.0f;  /* source row 2 col 1 */
    uint32_t vmscl = (0x3Cu << 26) | (4u << 23) | (0u << 16) | (0x20u << 8) | (1u << 15) | 0x20u;
    CHECK(sr_vfpu_interp(&s, vmscl) == SR_VFPU_COMPUTE, "vmscl.t scalar-in-dest computes");
    CHECK(s.v[0] == 3.0f * 3.0f, "vmscl.t row0col0 uses original scalar");
    CHECK(s.v[1] == 2.0f * 3.0f, "vmscl.t row0col1 uses original scalar");
    CHECK(s.v[4] == 5.0f * 3.0f, "vmscl.t row1col0 uses original scalar");
    CHECK(s.v[9] == 7.0f * 9.0f, "vmscl.t row2col1 sees clobbered scalar");
    if (s.v[0] != 3.0f * 3.0f || s.v[1] != 2.0f * 3.0f ||
        s.v[4] != 5.0f * 3.0f || s.v[9] != 7.0f * 9.0f) bad = 1;

    /* vmscl alias (Matrix1 which=1): scalar at register which (v[4] for which=1).
     * vs=0x20, vd=0x20. The scalar v[4] is overwritten by row 1 col 0, so the
     * last row reads the product through sr_vread. */
    setup_identity_compute(&s);
    s.v[0] = 4.0f;  /* source row 0 col 0 */
    s.v[1] = 3.0f;  /* source row 0 col 1 */
    s.v[4] = 6.0f;  /* scalar AND source row 1 col 0 */
    s.v[9] = 8.0f;  /* source row 2 col 1 */
    uint32_t vmscl_alias = (0x3Cu << 26) | (7u << 23) | (1u << 16) | (0x20u << 8) | (1u << 15) | 0x20u;
    CHECK(sr_vfpu_interp(&s, vmscl_alias) == SR_VFPU_COMPUTE, "vmscl alias scalar-in-dest computes");
    CHECK(s.v[0] == 4.0f * 6.0f, "vmscl alias row0col0 uses original scalar");
    CHECK(s.v[1] == 3.0f * 6.0f, "vmscl alias row0col1 uses original scalar");
    CHECK(s.v[4] == 6.0f * 6.0f, "vmscl alias row1col0 overwrites scalar");
    CHECK(s.v[9] == 8.0f * 36.0f, "vmscl alias row2col1 sees clobbered scalar");
    if (s.v[0] != 4.0f * 6.0f || s.v[1] != 3.0f * 6.0f ||
        s.v[4] != 6.0f * 6.0f || s.v[9] != 8.0f * 36.0f) bad = 1;

    return bad;
}

static int check_overlap_diff(void) {
    CpuState s0, s1, s2;
    int bad = 0;

    for (int c = 0; c < OVERLAP_DIFF_NCASES; c++) {
        uint32_t w = overlap_diff_cases[c].w;
        const char *name = overlap_diff_cases[c].name;
        int case_bad = 0;

        memset(&s0, 0, sizeof(s0));
        for (int i = 0; i < 128; i++) {
            int32_t m = (int32_t)((i * 37 + c * 13) % 8192) - 4096;
            s0.v[i] = (float)m / 32.0f;
        }
        sr_vfpu_thread_ctrl_init(&s0); /* identity prefixes, thread-entry RCX0-7 */
        s0.vfpuCtrl[3] = 0u;

        s1 = s0;
        s2 = s0;
        overlap_diff_run_codegen(&s1, c);
        int kind = sr_vfpu_interp(&s2, w);
        CHECK(kind == SR_VFPU_COMPUTE, "overlap diff interp executes");
        if (kind != SR_VFPU_COMPUTE) { case_bad = 1; }

        for (int i = 0; i < 128 && !case_bad; i++) {
            if (s1.vi[i] != s2.vi[i]) {
                fprintf(stderr,
                        "MISMATCH overlap diff case=%s op=0x%08x: v%d codegen=0x%08x (%g) interp=0x%08x (%g)\n",
                        name, w, i, s1.vi[i], s1.v[i], s2.vi[i], s2.v[i]);
                case_bad = 1;
                break;
            }
        }
        for (int i = 0; i < 16 && !case_bad; i++) {
            if (s1.vfpuCtrl[i] != s2.vfpuCtrl[i]) {
                fprintf(stderr,
                        "MISMATCH overlap diff case=%s op=0x%08x: vfpuCtrl[%d] codegen=0x%08x interp=0x%08x\n",
                        name, w, i, s1.vfpuCtrl[i], s2.vfpuCtrl[i]);
                case_bad = 1;
                break;
            }
        }
        if (case_bad) bad = 1;
    }
    return bad;
}

static int check_vhdp_signed_zero_corpus(void) {
    CpuState codegen, interp;
    setup_identity_compute(&codegen);
    /* The exotic vhdp.t case uses vs={v0,v1,v2}, vt={v16,v17,v18}, vd={v32}.
     * Every product is -0. IEEE chained addition preserves -0, while the
     * repaired ordered accumulator starts at +0 and produces +0. */
    codegen.vi[0] = 0x00000000u;
    codegen.vi[1] = 0x00000000u;
    codegen.vi[2] = 0x00000000u;
    codegen.vi[16] = 0x80000000u;
    codegen.vi[17] = 0x80000000u;
    codegen.vi[18] = 0x80000000u;
    codegen.vi[32] = 0xdeadbeefu;
    interp = codegen;

    overlap_diff_run_codegen(&codegen, OVERLAP_DIFF_VHDP_EXOTIC_INDEX);
    int kind = sr_vfpu_interp(&interp,
                              overlap_diff_cases[OVERLAP_DIFF_VHDP_EXOTIC_INDEX].w);
    CHECK(kind == SR_VFPU_COMPUTE, "vhdp signed-zero corpus executes in interpreter");
    CHECK(codegen.vi[32] == 0x00000000u,
          "ordered vhdp codegen emits positive zero for signed-zero corpus");
    CHECK(interp.vi[32] == 0x00000000u,
          "vhdp interpreter emits positive zero for signed-zero corpus");
    CHECK(codegen.vi[32] == interp.vi[32],
          "ordered vhdp codegen agrees with interpreter on signed-zero corpus");
    return codegen.vi[32] != interp.vi[32] || interp.vi[32] != 0x00000000u;
}

static float vfpu3_compare_expect(unsigned sub, float a, float b) {
    /* The stated result (issue #69 / #326) for non-NaN operands: vscmp -1/0/1, vsge and vslt
     * 1/0. Used only as the spec check, next to the lane-for-lane comparison. A NaN lane
     * follows the measured words that check_vfpu_compare_measured_words() pins. */
    if (sub == 5u) return (a < b) ? -1.0f : ((a > b) ? 1.0f : 0.0f);
    if (sub == 6u) return (a >= b) ? 1.0f : 0.0f;
    return (a < b) ? 1.0f : 0.0f;
}

static int check_vfpu3_compare_parity(void) {
    /* AOT codegen vs sr_vfpu_interp for the VFPU3 compare family (issue #326). Each vscmp,
     * vsge and vslt width (single, pair, triple, quad) runs through the generated body and
     * the production interpreter on identical state. The operand corpus holds ordinary values,
     * equal pairs, signed zeros and infinities; it holds no NaN. The console measured NaN only
     * for the five single-width NaN pairs of PSP-VFPU-CMP-001 (15 cells), which
     * check_vfpu_compare_measured_words() pins; other NaN pairs (NaN against NaN or an
     * infinity) stay unmeasured. Pass 0 gives every lane the same pair; pass 1 staggers the pairs
     * across lanes. Both lanes must leave every v[] lane and vfpuCtrl[] identical, and the
     * destination lanes must hold the stated result. */
    static const float corpus[] = {
        1.0f, -1.0f, 2.5f, -2.5f, 0.5f,   /* ordinary values */
        0.0f, -0.0f,                      /* signed zeros: equal under < and >= */
        INFINITY, -INFINITY,
    };
    enum { NC = (int)(sizeof(corpus) / sizeof(corpus[0])) };
    const int first = OVERLAP_DIFF_CMP_FIRST;
    const int count = OVERLAP_DIFF_CMP_COUNT;
    int bad = 0;

    for (int c = first; c < first + count; c++) {
        const uint32_t w = overlap_diff_cases[c].w;
        const char *name = overlap_diff_cases[c].name;
        const unsigned sub = (w >> 23) & 7u;
        const int n = vsize(w);
        uint8_t si[4], ti[4], di[4];
        vreg_idx((int)((w >> 8) & 0x7fu), n, si);
        vreg_idx((int)((w >> 16) & 0x7fu), n, ti);
        vreg_idx((int)(w & 0x7fu), n, di);

        for (int pass = 0; pass < 2; pass++) {
            int combo_bad = 0;
            for (int p = 0; p < NC * NC && !combo_bad; p++) {
                CpuState s0, s1, s2;
                float a[4], b[4];
                const int ai = p / NC, bi = p % NC;

                setup_identity_compute(&s0);
                /* Destination prefix active: saturate every lane to [-1,1]. That keeps -1/0/1 and
                 * 0/1 unchanged, and makes the consume back to identity visible in vfpuCtrl[].
                 * The S/T prefixes stay identity: no identity-valued non-0xe4 source prefix exists
                 * within the 20 prefix bits (any abs, constant or negate bit changes the value). */
                s0.vfpuCtrl[2] = 0xffu;
                for (int i = 0; i < 128; i++) s0.vi[i] = 0xa5a5a5a5u;
                for (int i = 0; i < n; i++) {
                    a[i] = pass == 0 ? corpus[ai] : corpus[(ai + i) % NC];
                    b[i] = pass == 0 ? corpus[bi] : corpus[(bi + 2 * i + 1) % NC];
                    s0.v[si[i]] = a[i];
                    s0.v[ti[i]] = b[i];
                }
                s1 = s0;
                s2 = s0;
                overlap_diff_run_codegen(&s1, c);
                if (sr_vfpu_interp(&s2, w) != SR_VFPU_COMPUTE) {
                    fprintf(stderr, "VFPU3 compare parity: %s op=0x%08x interpreter did not compute\n",
                            name, w);
                    combo_bad = 1;
                    break;
                }
                for (int i = 0; i < 128 && !combo_bad; i++) {
                    if (s1.vi[i] != s2.vi[i]) {
                        fprintf(stderr,
                                "MISMATCH vfpu3 compare parity case=%s op=0x%08x pass=%d pair=(%g,%g): "
                                "v%d codegen=0x%08x interp=0x%08x\n",
                                name, w, pass, a[0], b[0], i, s1.vi[i], s2.vi[i]);
                        combo_bad = 1;
                    }
                }
                for (int i = 0; i < 4 && !combo_bad; i++) {
                    if (s1.vfpuCtrl[i] != s2.vfpuCtrl[i]) {
                        fprintf(stderr,
                                "MISMATCH vfpu3 compare parity case=%s op=0x%08x pass=%d: "
                                "vfpuCtrl[%d] codegen=0x%08x interp=0x%08x\n",
                                name, w, pass, i, s1.vfpuCtrl[i], s2.vfpuCtrl[i]);
                        combo_bad = 1;
                    }
                }
                for (int i = 0; i < n && !combo_bad; i++) {
                    const float want = vfpu3_compare_expect(sub, a[i], b[i]);
                    if (s1.v[di[i]] != want || s2.v[di[i]] != want) {
                        fprintf(stderr,
                                "MISMATCH vfpu3 compare spec case=%s op=0x%08x pass=%d lane=%d "
                                "pair=(%g,%g): want %g codegen=%g interp=%g\n",
                                name, w, pass, i, a[i], b[i], want, s1.v[di[i]], s2.v[di[i]]);
                        combo_bad = 1;
                    }
                }
            }
            CHECK(!combo_bad, "VFPU3 compare: AOT codegen and sr_vfpu_interp agree on every width and operand pair");
            if (combo_bad) bad = 1;
        }
    }
    return bad;
}

static uint32_t vf2i(unsigned mode, unsigned scale) {
    /* Scalar vf2in/vf2iz/vf2iu/vf2id, vs=0 (physical v[0]), vd=1
     * (physical v[4]). Width bits 7/15 remain clear. */
    return (0x34u << 26) | ((16u + (mode & 3u)) << 21) |
           ((scale & 31u) << 16) | 1u;
}

static int check_vf2i_conversions(void) {
    static const struct {
        const char *name;
        uint32_t input;
        unsigned mode;
        unsigned scale;
        uint32_t expected;
    } vectors[] = {
        {"vf2in 0.5 ties even", 0x3f000000u, 0u, 0u, 0x00000000u},
        {"vf2in 1.5 ties even", 0x3fc00000u, 0u, 0u, 0x00000002u},
        {"vf2in 2.5 ties even", 0x40200000u, 0u, 0u, 0x00000002u},
        {"vf2in -1.5 ties even", 0xbfc00000u, 0u, 0u, 0xfffffffeu},
        {"vf2iz -2.75", 0xc0300000u, 1u, 0u, 0xfffffffeu},
        {"vf2iu -2.25", 0xc0100000u, 2u, 0u, 0xfffffffeu},
        {"vf2id -2.25", 0xc0100000u, 3u, 0u, 0xfffffffdu},
        {"vf2in scaled tie", 0x3f400000u, 0u, 1u, 0x00000002u},
        {"vf2iz largest below INTMAX", 0x3f7fffffu, 1u, 31u, 0x7fffff80u},
        {"vf2in positive overflow", 0x3f800000u, 0u, 31u, 0x7fffffffu},
        {"vf2in negative boundary", 0xbf800000u, 0u, 31u, 0x80000000u},
        {"vf2in positive infinity", 0x7f800000u, 0u, 0u, 0x7fffffffu},
        {"vf2in negative infinity", 0xff800000u, 0u, 0u, 0x80000000u},
        {"vf2in NaN", 0x7fc00000u, 0u, 0u, 0x7fffffffu},
        {"vf2in signaling NaN", 0x7f800001u, 0u, 0u, 0x7fffffffu},
    };
    static const int host_modes[] = {
        FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO,
    };
    CpuState s;
    const int saved_mode = fegetround();

    for (size_t m = 0; m < sizeof(host_modes) / sizeof(host_modes[0]); m++) {
        CHECK(fesetround(host_modes[m]) == 0, "host rounding mode is available for vf2i test");
        for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
            setup_identity_compute(&s);
            s.vi[0] = vectors[i].input;
            s.vi[4] = 0xa5a5a5a5u;
            CHECK(sr_vfpu_interp(&s, vf2i(vectors[i].mode, vectors[i].scale)) == SR_VFPU_COMPUTE,
                  "vf2i instruction executes through production fallback");
            CHECK(s.vi[4] == vectors[i].expected, vectors[i].name);
        }
    }
    setup_identity_compute(&s);
    s.vi[0] = 0x3fc00000u;
    s.vi[4] = 0xa5a5a5a5u;
    s.vfpuCtrl[2] = 1u << 8;
    CHECK(sr_vfpu_interp(&s, vf2i(0u, 0u)) == SR_VFPU_COMPUTE,
          "masked vf2i instruction executes through production fallback");
    CHECK(s.vi[4] == 0xa5a5a5a5u, "vf2i honors the destination write mask");
    CHECK(s.vfpuCtrl[0] == 0xe4u && s.vfpuCtrl[1] == 0xe4u && s.vfpuCtrl[2] == 0u,
          "masked vf2i consumes prefixes");
    if (saved_mode != -1) (void)fesetround(saved_mode);
    return 0;
}

static int check_reserved_cmov_form(void) {
    /* vcmov is VFPU4 (jump 21); imm3 7 is its reserved selector and must fail closed.
     * Major 0x1b sub-ops 6 and 7 are not vcmov forms (see check_vfpu3_compare_forms). */
    CpuState s;
    const uint32_t word = (0x34u << 26) | (21u << 21) | (7u << 16);
    int bad = 0;
    setup_state(&s);
    const CpuState before = s;
    int kind = sr_vfpu_interp(&s, word);
    if (kind != SR_VFPU_OTHER) {
        fprintf(stderr, "reserved vcmov word=0x%08x imm=%u kind=%d\n",
                word, (word >> 16) & 7, kind);
    }
    CHECK(kind == SR_VFPU_OTHER, "reserved vcmov imm3 fails closed");
    CHECK(memcmp(&s, &before, sizeof(s)) == 0, "reserved vcmov imm3 leaves state unchanged");
    return bad;
}

static uint32_t vfpu3_single(unsigned sub, unsigned vt, unsigned vs, unsigned vd) {
    /* Major 0x1b, bits 25..23 select the op; width bits 7/15 clear (.s form). */
    return (0x1bu << 26) | ((sub & 7u) << 23) | ((vt & 0x7fu) << 16) | ((vs & 0x7fu) << 8) |
           (vd & 0x7fu);
}

static int check_vfpu3_compare_forms(void) {
    /* Major 0x1b sub-ops 5, 6 and 7 are vscmp, vsge and vslt (issue #69 root cause: they
     * were decoded as vcmovt/vcmovf before 2026-10-10). Each compares S against T per lane
     * and writes a float: vscmp -1/0/1, vsge and vslt 1/0. The NaN rows use the operands a
     * PSP-3000 measured (PSP-VFPU-CMP-001): vsge and vslt write 0.0, and vscmp orders a
     * NaN by sign and magnitude, so +NaN against 1.5 writes +1.0 and 1.5 against +NaN
     * writes -1.0. vs=0 is physical v[0], vt=2 is v[8], vd=1 is v[4]. */
    static const struct {
        unsigned sub;
        uint32_t a, b, expect;
        const char *name;
    } cases[] = {
        {6u, 0x3f800000u, 0x00000000u, 0x3f800000u, "vsge.s 1 >= 0 writes 1.0"},
        {6u, 0x00000000u, 0x00000000u, 0x3f800000u, "vsge.s 0 >= 0 writes 1.0"},
        {6u, 0xbf800000u, 0x00000000u, 0x00000000u, "vsge.s -1 >= 0 writes 0.0"},
        {6u, 0x7fc00000u, 0x3fc00000u, 0x00000000u, "vsge.s NaN >= 1.5 writes 0.0 (PSP-VFPU-CMP-001)"},
        {7u, 0xbf800000u, 0x00000000u, 0x3f800000u, "vslt.s -1 < 0 writes 1.0"},
        {7u, 0x00000000u, 0x00000000u, 0x00000000u, "vslt.s 0 < 0 writes 0.0"},
        {7u, 0x3f800000u, 0x00000000u, 0x00000000u, "vslt.s 1 < 0 writes 0.0"},
        {7u, 0x3fc00000u, 0x7fc00000u, 0x00000000u, "vslt.s 1.5 < NaN writes 0.0 (PSP-VFPU-CMP-001)"},
        {5u, 0xc0000000u, 0x00000000u, 0xbf800000u, "vscmp.s -2 vs 0 writes -1.0"},
        {5u, 0x00000000u, 0x00000000u, 0x00000000u, "vscmp.s 0 vs 0 writes 0.0"},
        {5u, 0x40400000u, 0x00000000u, 0x3f800000u, "vscmp.s 3 vs 0 writes 1.0"},
        {5u, 0x7fc00000u, 0x3fc00000u, 0x3f800000u, "vscmp.s NaN vs 1.5 writes 1.0 (PSP-VFPU-CMP-001)"},
        {5u, 0x3fc00000u, 0x7fc00000u, 0xbf800000u, "vscmp.s 1.5 vs NaN writes -1.0 (PSP-VFPU-CMP-001)"},
        {5u, 0x00000001u, 0x00000000u, 0x00000000u,
         "vscmp.s +denormal vs 0 writes 0.0 (denormal flushed, PSP-HLE-ISSUES-001)"},
        {5u, 0x80000001u, 0x00000000u, 0x00000000u,
         "vscmp.s -denormal vs 0 writes 0.0 (denormal flushed, PSP-HLE-ISSUES-001)"},
    };
    CpuState s;
    int bad = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        setup_identity_compute(&s);
        s.vi[0] = cases[i].a;
        s.vi[8] = cases[i].b;
        s.vi[4] = 0xa5a5a5a5u;
        int kind = sr_vfpu_interp(&s, vfpu3_single(cases[i].sub, 2u, 0u, 1u));
        CHECK(kind == SR_VFPU_COMPUTE, "VFPU3 compare executes through production fallback");
        CHECK(s.vi[4] == cases[i].expect, cases[i].name);
        CHECK(s.vi[0] == cases[i].a && s.vi[8] == cases[i].b, "VFPU3 compare leaves sources intact");
        CHECK(s.vfpuCtrl[0] == 0xe4u && s.vfpuCtrl[1] == 0xe4u && s.vfpuCtrl[2] == 0u,
              "VFPU3 compare consumes prefixes");
    }
    /* The destination write mask still applies. */
    setup_identity_compute(&s);
    s.vi[0] = 0x3f800000u;
    s.vi[8] = 0u;
    s.vi[4] = 0xa5a5a5a5u;
    s.vfpuCtrl[2] = 1u << 8;
    CHECK(sr_vfpu_interp(&s, vfpu3_single(6u, 2u, 0u, 1u)) == SR_VFPU_COMPUTE,
          "masked vsge.s executes through production fallback");
    CHECK(s.vi[4] == 0xa5a5a5a5u, "vsge.s honors the destination write mask");
    return bad;
}

static int check_mfvc_zero_is_noop(void) {
    /* mfv/mfvc into $zero discards the value (MIPS r0), whatever register is read, including
     * the out-of-range control number 255 of the `mfvc $zero, 255` sync idiom (0x486000ff).
     * A real GPR destination with an out-of-range control register still fails closed. */
    CpuState s;
    int bad = 0;
    setup_state(&s);
    const CpuState before = s;
    int kind = sr_vfpu_interp(&s, 0x486000ffu);
    CHECK(kind == SR_VFPU_COMPUTE, "mfvc $zero, 255 completes as a no-op");
    CHECK(memcmp(&s, &before, sizeof(s)) == 0, "mfvc $zero, 255 leaves state unchanged");
    setup_state(&s);
    kind = sr_vfpu_interp(&s, 0x486200ffu);  /* mfvc $v0, 255 */
    CHECK(kind == SR_VFPU_OTHER, "mfvc to a real GPR from control register 255 fails closed");
    return bad;
}

static int check_vasin_measured_domain_words(void) {
    /* The 14 raw-word results from two byte-identical PSP runs; see issue #69
     * and the exact fixture/method in docs/HARDWARE_ORACLE.md. */
    static const struct {
        uint32_t input;
        uint32_t result;
    } cases[] = {
        {0xBF800000u, 0xBF800000u},
        {0x3F800000u, 0x3F800000u},
        {0xBF800001u, 0xFF800001u},
        {0x3F800001u, 0x7F800001u},
        {0x3F80000Bu, 0x7F800001u},
        {0xBF80000Bu, 0xFF800001u},
        {0xBF80DABCu, 0xFF800001u},
        {0xBF82026Au, 0xFF800001u},
        {0xBF8FA2B7u, 0xFF800001u},
        {0xBF9A419Cu, 0xFF800001u},
        {0xBFB63DDAu, 0xFF800001u},
        {0xBFFB5A51u, 0xFF800001u},
        {0xBFFFFE00u, 0xFF800001u},
        {0xC0000000u, 0xFF800001u},
    };
    int bad = 0;

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        float input;
        float output;
        uint32_t actual;
        memcpy(&input, &cases[i].input, sizeof(input));
        output = sr_vfpu_asin(input);
        memcpy(&actual, &output, sizeof(actual));
        g_checks++;
        if (actual != cases[i].result) {
            g_failures++;
            bad = 1;
            fprintf(stderr,
                    "FAIL: measured VASIN word[%u] input=0x%08x "
                    "expected=0x%08x actual=0x%08x\n",
                    i, cases[i].input, cases[i].result, actual);
        }
    }
    return bad;
}

static int find_overlap_diff_case(const char *name) {
    for (int c = 0; c < OVERLAP_DIFF_NCASES; c++) {
        if (strcmp(overlap_diff_cases[c].name, name) == 0) return c;
    }
    return -1;
}

static int check_vfpu_compare_measured_words(void) {
    /* PSP-VFPU-CMP-001: the 45 result words a PSP-3000 (6.61, 2026-10-15, probe build
     * e45e667b) returned for vscmp.s, vsge.s and vslt.s S002, S000, S001 on 15 operand
     * pairs; docs/HARDWARE_ORACLE.md indexes the run. Both execution lanes run the probe's
     * own words: the AOT body tools/codegen.py emits (via the overlap-diff header) and the
     * production interpreter. vsge/vslt write 0.0 for every NaN pair; vscmp orders a NaN
     * by sign and magnitude (+NaN above, -NaN below), which the interpreter wrote as 0.0
     * before this measurement. */
    static const char *const op_case[3] = {
        "vscmp-s-measured", "vsge-s-measured", "vslt-s-measured",
    };
    static const struct {
        const char *name;
        uint32_t a, b;
        uint32_t psp[3]; /* measured word per op: vscmp.s, vsge.s, vslt.s */
    } pairs[] = {
        {"lt", 0x3fc00000u, 0x40200000u, {0xbf800000u, 0x00000000u, 0x3f800000u}},
        {"eq", 0x3fc00000u, 0x3fc00000u, {0x00000000u, 0x3f800000u, 0x00000000u}},
        {"gt", 0x40200000u, 0x3fc00000u, {0x3f800000u, 0x3f800000u, 0x00000000u}},
        {"zero-pos-neg", 0x00000000u, 0x80000000u, {0x00000000u, 0x3f800000u, 0x00000000u}},
        {"zero-neg-pos", 0x80000000u, 0x00000000u, {0x00000000u, 0x3f800000u, 0x00000000u}},
        {"nan-left-quiet", 0x7fc00000u, 0x3fc00000u, {0x3f800000u, 0x00000000u, 0x00000000u}},
        {"nan-right-quiet", 0x3fc00000u, 0x7fc00000u, {0xbf800000u, 0x00000000u, 0x00000000u}},
        {"nan-left-signal", 0x7f800001u, 0x3fc00000u, {0x3f800000u, 0x00000000u, 0x00000000u}},
        {"nan-right-signal", 0x3fc00000u, 0x7f800001u, {0xbf800000u, 0x00000000u, 0x00000000u}},
        {"nan-left-negative", 0xffc00000u, 0x3fc00000u, {0xbf800000u, 0x00000000u, 0x00000000u}},
        {"inf-pos-neg", 0x7f800000u, 0xff800000u, {0x3f800000u, 0x3f800000u, 0x00000000u}},
        {"inf-neg-pos", 0xff800000u, 0x7f800000u, {0xbf800000u, 0x00000000u, 0x3f800000u}},
        {"inf-pos-pos", 0x7f800000u, 0x7f800000u, {0x00000000u, 0x3f800000u, 0x00000000u}},
        {"inf-neg-neg", 0xff800000u, 0xff800000u, {0x00000000u, 0x3f800000u, 0x00000000u}},
        {"inf-pos-finite", 0x7f800000u, 0x3fc00000u, {0x3f800000u, 0x3f800000u, 0x00000000u}},
    };
    int bad = 0;
    for (int op = 0; op < 3; op++) {
        const int c = find_overlap_diff_case(op_case[op]);
        CHECK(c >= 0, "measured VFPU compare word is in the overlap-diff header");
        if (c < 0) { bad = 1; continue; }
        const uint32_t w = overlap_diff_cases[c].w;
        uint8_t si[4], ti[4], di[4];
        vreg_idx((int)((w >> 8) & 0x7fu), 1, si);
        vreg_idx((int)((w >> 16) & 0x7fu), 1, ti);
        vreg_idx((int)(w & 0x7fu), 1, di);
        for (size_t p = 0; p < sizeof(pairs) / sizeof(pairs[0]); p++) {
            CpuState aot, interp;
            setup_identity_compute(&aot);
            aot.vi[si[0]] = pairs[p].a;
            aot.vi[ti[0]] = pairs[p].b;
            aot.vi[di[0]] = 0x7a5a5a5au; /* the probe's sentinel */
            interp = aot;
            overlap_diff_run_codegen(&aot, c);
            const int kind = sr_vfpu_interp(&interp, w);
            const uint32_t want = pairs[p].psp[op];
            g_checks++;
            if (kind != SR_VFPU_COMPUTE || aot.vi[di[0]] != want || interp.vi[di[0]] != want ||
                memcmp(aot.vi, interp.vi, sizeof(aot.vi)) != 0 ||
                memcmp(aot.vfpuCtrl, interp.vfpuCtrl, sizeof(aot.vfpuCtrl)) != 0) {
                g_failures++;
                bad = 1;
                fprintf(stderr,
                        "FAIL: PSP-VFPU-CMP-001 %s-%s a=0x%08x b=0x%08x psp=0x%08x "
                        "aot=0x%08x interp=0x%08x kind=%d\n",
                        op_case[op], pairs[p].name, pairs[p].a, pairs[p].b, want,
                        aot.vi[di[0]], interp.vi[di[0]], kind);
            }
        }
    }
    return bad;
}

static int check_vscmp_hle_issues_cells(void) {
    /* PSP-HLE-ISSUES-001 (PSP-3000, 6.61, 2026-10-24, probe case hle-issues-batch, PR #1269;
     * captured from da97b2c0 without a runner envelope, so not acceptance-eligible): the
     * probe's own vscmp.s/.p/.t/.q C200, C000, C100 and vscmp.q C200, C000, C000 words over
     * its six operand sets (fixtures/psp_oracle/probe_hle_issues.c). docs/HARDWARE_ORACLE.md
     * states the result of all 30 cells, identical on both passes: per lane, -1, 0 or +1 in
     * sign-magnitude integer order of the bit patterns, with denormals flushed to zero,
     * +0 equal to -0 and NaNs above infinity by bit pattern, at every width, and VFPU_CC not
     * written. The want[] words below are that rule applied to each lane; the same-register
     * form compares every lane with itself and writes 0.0. Both execution lanes run each
     * word: the AOT body tools/codegen.py emits (via the overlap-diff header) and the
     * production interpreter. Lanes past the width and every other register keep their
     * values. The denorm-finite set failed before the denormal flush: the host compare
     * ordered 0x00000001 above 0 and 0x80000001 below it. */
    static const struct {
        const char *name;
        uint32_t vs[4], vt[4], want[4];
    } sets[] = {
        {"qnan-inf", {0x7fc00000u, 0x7fc00000u, 0x7f800000u, 0xff800000u},
         {0x7fc00000u, 0x7f800000u, 0x7fc00000u, 0x7fc00000u},
         {0x00000000u, 0x3f800000u, 0xbf800000u, 0xbf800000u}},
        {"nan-kinds", {0x7f800001u, 0xffc00000u, 0x7fffffffu, 0x7f800001u},
         {0x3f800000u, 0xbf800000u, 0x7fffffffu, 0xff800001u},
         {0x3f800000u, 0xbf800000u, 0x00000000u, 0x3f800000u}},
        {"inf-inf", {0x7f800000u, 0xff800000u, 0x7f800000u, 0xff800000u},
         {0x7f800000u, 0xff800000u, 0xff800000u, 0x7f800000u},
         {0x00000000u, 0x00000000u, 0x3f800000u, 0xbf800000u}},
        {"zero-nan", {0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u},
         {0x80000000u, 0x00000000u, 0x7fc00000u, 0xffc00000u},
         {0x00000000u, 0x00000000u, 0xbf800000u, 0x3f800000u}},
        {"denorm-finite", {0x00000001u, 0x80000001u, 0x40400000u, 0x40000000u},
         {0x00000000u, 0x00000000u, 0x40000000u, 0x40400000u},
         {0x00000000u, 0x00000000u, 0x3f800000u, 0xbf800000u}},
        {"nan-mixed", {0x7f800001u, 0x3f800000u, 0x7fc00000u, 0xff800000u},
         {0xff800001u, 0x7f800001u, 0xff800000u, 0x7fc00000u},
         {0x3f800000u, 0xbf800000u, 0x3f800000u, 0xbf800000u}},
    };
    static const char *const forms[] = {
        "vscmp-s-issues", "vscmp-p-issues", "vscmp-t-issues", "vscmp-q-issues",
        "vscmp-q-same-issues",
    };
    int bad = 0;
    for (size_t f = 0; f < sizeof(forms) / sizeof(forms[0]); f++) {
        const int c = find_overlap_diff_case(forms[f]);
        CHECK(c >= 0, "PSP-HLE-ISSUES-001 vscmp word is in the overlap-diff header");
        if (c < 0) { bad = 1; continue; }
        const uint32_t w = overlap_diff_cases[c].w;
        const int n = vsize(w);
        const int same = ((w >> 8) & 0x7fu) == ((w >> 16) & 0x7fu);
        uint8_t si[4], ti[4], di[4];
        vreg_idx((int)((w >> 8) & 0x7fu), n, si);
        vreg_idx((int)((w >> 16) & 0x7fu), n, ti);
        vreg_idx((int)(w & 0x7fu), n, di);
        for (size_t p = 0; p < sizeof(sets) / sizeof(sets[0]); p++) {
            CpuState before, aot, interp;
            setup_identity_compute(&before);
            for (int i = 0; i < 128; i++) before.vi[i] = 0x3f800000u | ((uint32_t)i << 16);
            before.vfpuCtrl[3] = 0x5au; /* VFPU_CC preset: vscmp must leave it */
            for (int l = 0; l < n; l++) {
                before.vi[si[l]] = sets[p].vs[l];
                if (!same) before.vi[ti[l]] = sets[p].vt[l];
            }
            aot = before;
            interp = before;
            overlap_diff_run_codegen(&aot, c);
            const int kind = sr_vfpu_interp(&interp, w);
            CpuState want = before;
            for (int l = 0; l < n; l++) want.vi[di[l]] = same ? 0x00000000u : sets[p].want[l];
            g_checks++;
            if (kind != SR_VFPU_COMPUTE || memcmp(aot.vi, want.vi, sizeof(want.vi)) != 0 ||
                memcmp(interp.vi, want.vi, sizeof(want.vi)) != 0 || aot.vfpuCtrl[3] != 0x5au ||
                memcmp(aot.vfpuCtrl, interp.vfpuCtrl, sizeof(aot.vfpuCtrl)) != 0) {
                g_failures++;
                bad = 1;
                for (int l = 0; l < n; l++) {
                    fprintf(stderr,
                            "FAIL: PSP-HLE-ISSUES-001 %s-%s lane %d a=0x%08x b=0x%08x "
                            "psp=0x%08x aot=0x%08x interp=0x%08x kind=%d\n",
                            forms[f], sets[p].name, l, before.vi[si[l]], before.vi[ti[l]],
                            want.vi[di[l]], aot.vi[di[l]], interp.vi[di[l]], kind);
                }
            }
        }
    }
    return bad;
}

/* One vsgn/vbfy/vt4444 cell: the instruction word, the S prefix in force, the source
 * lanes and the expected destination lanes. `lanes` counts the destination lanes the
 * form writes; `measured` is 1 when every expected word is a PSP-3000 reading. */
struct vfpu9_cell {
    const char *name;
    uint32_t w, sprefix, src[4], want[4];
    int lanes, measured;
};

static int check_psp_measured_vfpu9(void) {
    /* PSP-3000, 2026-10-24 (issue #326): vsgn.s, vbfy1.p/.q, vbfy2.q and vt4444.q
     * measured from a fresh thread, 159 cells under no prefix, negate-all (vpfxs
     * 0xdc0f00e4) and wzyx with lanes 0/2 negated (0xdc05001b). Rows with measured=1
     * are words the PSP returned. Rows with measured=0 follow the rules those cells fix
     * (an off-tree replay of the production interpreter matches all 159 cells); they pin
     * shapes such as vbfy2 that no measured row below covers on its own. */
    static const struct vfpu9_cell cells[] = {
        /* vsgn: the raw sign bit decides for NaN and Inf; +-0 and denormals give +0. */
        {"vsgn.s +denormal", 0xd04a0405u, 0xe4u, {0x00000001u}, {0x00000000u}, 1, 1},
        {"vsgn.s +qNaN", 0xd04a0405u, 0xe4u, {0x7fc00000u}, {0x3f800000u}, 1, 1},
        {"vsgn.s -qNaN", 0xd04a0405u, 0xe4u, {0xffc00000u}, {0xbf800000u}, 1, 1},
        {"vsgn.s sNaN", 0xd04a0405u, 0xe4u, {0x7f800001u}, {0x3f800000u}, 1, 1},
        {"vsgn.s -0", 0xd04a0405u, 0xe4u, {0x80000000u}, {0x00000000u}, 1, 1},
        {"vsgn.s -denormal", 0xd04a0020u, 0xe4u, {0x80000001u}, {0x00000000u}, 1, 0},
        {"vsgn.s -Inf", 0xd04a0020u, 0xe4u, {0xff800000u}, {0xbf800000u}, 1, 0},
        /* vbfy1.q: inf - inf and NaN operands give the canonical 0x7f800001. */
        {"vbfy1.q specials", 0xd0428080u, 0xe4u,
         {0x7f800000u, 0xff800000u, 0x7fc00000u, 0x00000000u},
         {0x7f800001u, 0x7f800000u, 0x7f800001u, 0x7f800001u}, 4, 1},
        /* Forced negate OR-ed into the user S prefix: negate-all on (1,2,3,4) is
         * (+1,-1,+1,-1), not the butterfly of (-1,-2,-3,-4). */
        {"vbfy1.q 1234 negate-all", 0xd0428080u, 0xf00e4u,
         {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u},
         {0x3f800000u, 0xbf800000u, 0x3f800000u, 0xbf800000u}, 4, 1},
        /* Denormal operands flush to a zero of their own sign: lane 3 is
         * -(0x00000001) + -0 = -0 (measured); lane 2 is -0 + 0 = +0. */
        {"vbfy1.q denormal flush", 0xd0428183u, 0xe4u,
         {0x3f800000u, 0x40000000u, 0x80000000u, 0x00000001u},
         {0x40400000u, 0xbf800000u, 0x00000000u, 0x80000000u}, 4, 0},
        /* A user S swizzle reaching outside the pair writes +0 to that lane. */
        {"vbfy1.p 12 wzyx", 0xd0420183u, 0x5001bu,
         {0x3f800000u, 0x40000000u}, {0x00000000u, 0x00000000u}, 2, 1},
        {"vbfy1.p 12", 0xd0420183u, 0xe4u,
         {0x3f800000u, 0x40000000u}, {0x40400000u, 0xbf800000u}, 2, 0},
        {"vbfy2.q 1234", 0xd0438183u, 0xe4u,
         {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u},
         {0x40800000u, 0x40c00000u, 0xc0000000u, 0xc0000000u}, 4, 0},
        /* vt4444.q packs a quad into a pair; the S prefix acts on raw words. */
        {"vt4444.q listed", 0xd0599090u, 0xe4u,
         {0x00000000u, 0xffffffffu, 0x12345678u, 0x80ff00f0u},
         {0xffff0000u, 0x8f0f1357u}, 2, 1},
        {"vt4444.q listed negate-all", 0xd0599090u, 0xf00e4u,
         {0x00000000u, 0xffffffffu, 0x12345678u, 0x80ff00f0u},
         {0x7fff8000u, 0x0f0f9357u}, 2, 1},
    };
    int bad = 0;
    for (unsigned c = 0; c < sizeof(cells) / sizeof(cells[0]); c++) {
        const struct vfpu9_cell *k = &cells[c];
        const int n = vsize(k->w);
        uint8_t si[4], di[4];
        CpuState s;
        setup_identity_compute(&s);
        for (int i = 0; i < 128; i++) s.vi[i] = 0x3f800000u | ((uint32_t)i << 16);
        s.vfpuCtrl[0] = k->sprefix;
        s.vfpuCtrl[3] = 0x15u;
        vreg_idx((int)((k->w >> 8) & 0x7fu), n, si);
        for (int i = 0; i < n; i++) s.vi[si[i]] = k->src[i];
        CpuState before = s;
        vreg_idx((int)(k->w & 0x7fu), k->lanes, di);
        const int kind = sr_vfpu_interp(&s, k->w);
        int ok = kind == SR_VFPU_COMPUTE && s.vfpuCtrl[3] == 0x15u && s.vfpuCtrl[0] == 0xe4u;
        for (int i = 0; i < k->lanes; i++) {
            ok &= s.vi[di[i]] == k->want[i];
            before.vi[di[i]] = s.vi[di[i]];
        }
        /* Only the destination lanes change; the prefix is consumed, CC is untouched. */
        before.vfpuCtrl[0] = 0xe4u;
        ok &= memcmp(s.vi, before.vi, sizeof(s.vi)) == 0;
        ok &= memcmp(s.vfpuCtrl, before.vfpuCtrl, sizeof(s.vfpuCtrl)) == 0;
        g_checks++;
        if (!ok) {
            g_failures++;
            bad = 1;
            fprintf(stderr, "FAIL: PSP-3000 VFPU9 %s (%s) kind=%d got", k->name,
                    k->measured ? "measured" : "rule", kind);
            for (int i = 0; i < k->lanes; i++) fprintf(stderr, " %08x", s.vi[di[i]]);
            fprintf(stderr, "\n");
        }
    }
    /* Sizes the measurement does not support stay fail-closed with no state change. */
    static const uint32_t rejected[] = {
        0xd0420000u,  /* vbfy1.s */
        0xd0428000u,  /* vbfy1.t */
        0xd0430080u,  /* vbfy2.p */
        0xd0590080u,  /* vt4444.p */
        0xd0598000u,  /* vt4444.t */
    };
    for (unsigned r = 0; r < sizeof(rejected) / sizeof(rejected[0]); r++) {
        CpuState s;
        setup_state(&s);
        const CpuState before = s;
        const int kind = sr_vfpu_interp(&s, rejected[r]);
        CHECKB(bad, kind == SR_VFPU_OTHER && memcmp(&s, &before, sizeof(s)) == 0,
               "unmeasured vbfy/vt4444 width fails closed without consuming prefixes");
    }
    /* RCX0-7 at thread entry, read back through mfvc controls 136..143. */
    {
        static const uint32_t rcx[8] = {0x3f800001u, 0x3f800002u, 0x3f800004u, 0x3f800008u,
                                        0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u};
        CpuState s;
        memset(&s, 0, sizeof(s));
        sr_vfpu_thread_ctrl_init(&s);
        int ok = s.vfpuCtrl[0] == 0xe4u && s.vfpuCtrl[1] == 0xe4u && s.vfpuCtrl[2] == 0u;
        for (int i = 0; i < 8; i++) {
            ok &= sr_vfpu_interp(&s, cop2_transfer(3, 9, 136 + i)) == SR_VFPU_COMPUTE;
            ok &= s.r[9] == rcx[i];
        }
        CHECKB(bad, ok, "thread-entry RCX0-7 read back the PSP-3000 measured words");
    }
    return bad;
}

static int check_vrexp2_measured_words(void) {
    /* PSP-VFPU-TRANSCENDENTAL-001: the 63 words a PSP-3000 (6.61, 2026-10-15, probe build
     * 22b027f6) returned for vrexp2.s S002, S000, as {input, result} pairs: 20 special
     * encodings followed by 43 evenly spaced exponents; docs/HARDWARE_ORACLE.md indexes the
     * run. VREXP2 is exp2(-x): 1.0 for zero, 2.0 for -1.0 and 0.5 (0x3efffffc) for +1.0. Both
     * execution lanes run the probe's own words: the AOT body tools/codegen.py emits (via the
     * overlap-diff header) and the production interpreter. Both computed -exp2(x) before this
     * measurement, negative for every finite input. */
    static const uint32_t pairs[63][2] = {
        {0x00000000u, 0x3f800000u}, {0x80000000u, 0x3f800000u}, {0x00000001u, 0x3f800000u},
        {0x80000001u, 0x3f800000u}, {0x3f800000u, 0x3efffffcu}, {0xbf800000u, 0x40000000u},
        {0x3f000000u, 0x3f3504f0u}, {0xbf000000u, 0x3fb504f0u}, {0x40000000u, 0x3e7ffffcu},
        {0xc0000000u, 0x40800000u}, {0x00800000u, 0x3f7ffffcu}, {0x80800000u, 0x3f800000u},
        {0x7f000000u, 0x00000000u}, {0xff000000u, 0x7f800000u}, {0x7f800000u, 0x00000000u},
        {0xff800000u, 0x7f800000u}, {0x7fc00000u, 0x7f800001u}, {0xffc00000u, 0x7f800001u},
        {0x7f800001u, 0x7f800001u}, {0xff800001u, 0x7f800001u}, {0xc1760000u, 0x4725fed4u},
        {0xc16a0000u, 0x46c56728u}, {0xc15e0000u, 0x466ac0c4u}, {0xc1520000u, 0x460b95c0u},
        {0xc1460000u, 0x45a5fed4u}, {0xc13a0000u, 0x45456728u}, {0xc12e0000u, 0x44eac0c4u},
        {0xc1220000u, 0x448b95c0u}, {0xc1160000u, 0x4425fed4u}, {0xc10a0000u, 0x43c56728u},
        {0xc0fc0000u, 0x436ac0c4u}, {0xc0e40000u, 0x430b95c0u}, {0xc0cc0000u, 0x42a5fed4u},
        {0xc0b40000u, 0x42456728u}, {0xc09c0000u, 0x41eac0c4u}, {0xc0840000u, 0x418b95c0u},
        {0xc0580000u, 0x4125fed4u}, {0xc0280000u, 0x40c56728u}, {0xbff00000u, 0x406ac0c4u},
        {0xbf900000u, 0x400b95c0u}, {0xbec00000u, 0x3fa5fed4u}, {0x3ec00000u, 0x3f456728u},
        {0x3f900000u, 0x3eeac0c4u}, {0x3ff00000u, 0x3e8b95c0u}, {0x40280000u, 0x3e25fed4u},
        {0x40580000u, 0x3dc56728u}, {0x40840000u, 0x3d6ac0c4u}, {0x409c0000u, 0x3d0b95c0u},
        {0x40b40000u, 0x3ca5fed4u}, {0x40cc0000u, 0x3c456728u}, {0x40e40000u, 0x3beac0c4u},
        {0x40fc0000u, 0x3b8b95c0u}, {0x410a0000u, 0x3b25fed4u}, {0x41160000u, 0x3ac56728u},
        {0x41220000u, 0x3a6ac0c4u}, {0x412e0000u, 0x3a0b95c0u}, {0x413a0000u, 0x39a5fed4u},
        {0x41460000u, 0x39456728u}, {0x41520000u, 0x38eac0c4u}, {0x415e0000u, 0x388b95c0u},
        {0x416a0000u, 0x3825fed4u}, {0x41760000u, 0x37c56728u}, {0x41810000u, 0x376ac0c4u},
    };
    const int c = find_overlap_diff_case("vrexp2-s-measured");
    CHECK(c >= 0, "measured vrexp2 word is in the overlap-diff header");
    if (c < 0) return 1;
    const uint32_t w = overlap_diff_cases[c].w;
    uint8_t si[4], di[4];
    vreg_idx((int)((w >> 8) & 0x7fu), 1, si);
    vreg_idx((int)(w & 0x7fu), 1, di);
    int bad = 0;
    for (unsigned p = 0; p < sizeof(pairs) / sizeof(pairs[0]); p++) {
        CpuState aot, interp;
        setup_identity_compute(&aot);
        aot.vi[si[0]] = pairs[p][0];
        aot.vi[di[0]] = 0x7a5a5a5au; /* the probe's sentinel */
        interp = aot;
        overlap_diff_run_codegen(&aot, c);
        const int kind = sr_vfpu_interp(&interp, w);
        g_checks++;
        if (kind != SR_VFPU_COMPUTE || aot.vi[di[0]] != pairs[p][1] ||
            interp.vi[di[0]] != pairs[p][1]) {
            g_failures++;
            bad = 1;
            fprintf(stderr,
                    "FAIL: PSP-VFPU-TRANSCENDENTAL-001 vrexp2 in=0x%08x psp=0x%08x "
                    "aot=0x%08x interp=0x%08x kind=%d\n",
                    pairs[p][0], pairs[p][1], aot.vi[di[0]], interp.vi[di[0]], kind);
        }
    }
    return bad;
}

/* RCX0-7 image of a random-unit state as probe_hle_issues.c writes it: Ck split into
 * the low halves of RCXk and RCX(k+4), carry nibble k in bits 16-19 of RCXk, 0x3f8 above. */
static void vrndi_probe_image(uint32_t *rcx, const uint32_t c[4], uint32_t carry) {
    for (unsigned k = 0; k < 8u; k++) {
        const uint32_t half = (c[k & 3u] >> (k < 4u ? 0u : 16u)) & 0xffffu;
        rcx[k] = 0x3f800000u | (((carry >> (4u * k)) & 0xfu) << 16) | half;
    }
}

/* Run one vrndi.s word in the AOT lane (overlap-diff case c, skipped when c < 0) and the
 * interpreter from `before`; both must end in `want`. */
static int vrndi_lanes_match(int c, uint32_t w, int lane, const CpuState *before,
                             const CpuState *want, const char *what) {
    CpuState aot = *before, interp = *before;
    const int kind = sr_vfpu_interp(&interp, w);
    int ok = kind == SR_VFPU_COMPUTE && memcmp(interp.vi, want->vi, sizeof(want->vi)) == 0 &&
             memcmp(interp.vfpuCtrl, want->vfpuCtrl, sizeof(want->vfpuCtrl)) == 0;
    if (c >= 0) {
        overlap_diff_run_codegen(&aot, c);
        ok &= memcmp(aot.vi, want->vi, sizeof(want->vi)) == 0 &&
              memcmp(aot.vfpuCtrl, want->vfpuCtrl, sizeof(want->vfpuCtrl)) == 0;
    }
    g_checks++;
    if (!ok) {
        g_failures++;
        fprintf(stderr, "FAIL: vrndi.s 0x%08x %s kind=%d value aot=0x%08x interp=0x%08x "
                "want=0x%08x; RCX0 aot=0x%08x interp=0x%08x want=0x%08x\n", w, what, kind,
                aot.vi[lane], interp.vi[lane], want->vi[lane], aot.vfpuCtrl[8],
                interp.vfpuCtrl[8], want->vfpuCtrl[8]);
    }
    return !ok;
}

static int check_vrndi_rule_cells(void) {
    /* vrndi.s, issue #326. docs/HARDWARE_ORACLE.md (PSP-HLE-ISSUES-001, PSP-3000, 6.61,
     * 2026-10-24, probe case hle-issues-batch, not acceptance-eligible) records the step
     * rule that reproduces all 1792 rcar/rcbig rows: one vrndi.s S000 from RCX states the
     * probe wrote (C0 0x2468ace0, C1 0x13579bdf, C2/C3/carry from its grids). The raw rows
     * stay private, so these cells are that rule applied to twelve of the probe's own
     * states (carry-out 0, 1 and 2; carries in all eight nibbles), computed outside the
     * runtime: the drawn word and RCX0-7 after the step. Both lanes run each word -- the
     * AOT body tools/codegen.py emits (via the overlap-diff header) and the production
     * interpreter -- into S000 (the probe word) and S323. Only the destination lane and
     * RCX0-7 change; VFPU_CC and the prefixes are left. Before the step was implemented,
     * the AOT raised Unsupported and the interpreter answered SR_VFPU_OTHER. */
    static const uint32_t c2s[8] = {0x00000000u, 0x00000001u, 0x7fffffffu, 0x80000000u,
                                    0xfffffffeu, 0xffffffffu, 0x907f6e50u, 0x5a5a5a5au};
    static const uint32_t c3s[8] = {0x00000000u, 0x00000001u, 0x7fffffffu, 0x80000000u,
                                    0xc0000000u, 0xffffffffu, 0xc4d5e6d8u, 0x89abcdefu};
    static const uint32_t carries[2][7] = {
        {0x0u, 0x1u, 0x2u, 0x3u, 0xfu, 0x10u, 0xffffu},
        {0x00010000u, 0x40000000u, 0x7fff0000u, 0x80000000u, 0x12345678u, 0xfffffffcu,
         0xffffffffu},
    };
    static const struct {
        const char *name;
        unsigned i, j, k, big;
        uint32_t value, after[8];
    } cells[] = {
        {"rcar-0-0-0", 0u, 0u, 0u, 0u, 0x0b27424au,
         {0x3f80cf61u, 0x3f8072e9u, 0x3f800000u, 0x3f800000u, 0x3f802579u, 0x3f80e5adu,
          0x3f800000u, 0x3f800000u}},
        {"rcar-1-1-1", 1u, 1u, 1u, 0u, 0x0b27424eu,
         {0x3f80cf61u, 0x3f8072e9u, 0x3f800001u, 0x3f800004u, 0x3f802579u, 0x3f80e5adu,
          0x3f800000u, 0x3f800000u}},
        {"rcar-2-3-6", 2u, 3u, 6u, 0u, 0x8b284248u,
         {0x3f80cf61u, 0x3f8072e9u, 0x3f800000u, 0x3f80fffeu, 0x3f802579u, 0x3f80e5adu,
          0x3f808000u, 0x3f808000u}},
        {"rcar-5-5-6 (carry out 1)", 5u, 5u, 6u, 0u, 0x0b284246u,
         {0x3f81cf61u, 0x3f8072e9u, 0x3f80ffffu, 0x3f80fffcu, 0x3f802579u, 0x3f80e5adu,
          0x3f80ffffu, 0x3f800000u}},
        {"rcar-3-4-4", 3u, 4u, 4u, 0u, 0x0b274259u,
         {0x3f80cf61u, 0x3f8072e9u, 0x3f800000u, 0x3f80000fu, 0x3f802579u, 0x3f80e5adu,
          0x3f80c000u, 0x3f800000u}},
        {"rcar-6-7-5", 6u, 7u, 5u, 0u, 0xaefe4c88u,
         {0x3f80cf61u, 0x3f8072e9u, 0x3f80cdefu, 0x3f800a3eu, 0x3f802579u, 0x3f80e5adu,
          0x3f8089abu, 0x3f80a3d7u}},
        {"rcbig-0-0-0", 0u, 0u, 0u, 1u, 0x0b28424au,
         {0x3f80cf61u, 0x3f8072e9u, 0x3f800000u, 0x3f800000u, 0x3f802579u, 0x3f80e5adu,
          0x3f800000u, 0x3f800001u}},
        {"rcbig-5-5-6 (carry out 2)", 5u, 5u, 6u, 1u, 0x0b274246u,
         {0x3f82cf61u, 0x3f8072e9u, 0x3f80ffffu, 0x3f80fffcu, 0x3f802579u, 0x3f80e5adu,
          0x3f80ffffu, 0x3f80ffffu}},
        {"rcbig-7-6-4", 7u, 6u, 4u, 1u, 0x0161c0ccu,
         {0x3f80cf61u, 0x3f8072e9u, 0x3f80e6d8u, 0x3f807e82u, 0x3f802579u, 0x3f80e5adu,
          0x3f80c4d5u, 0x3f80f63au}},
        {"rcbig-4-2-5 (carry out 2)", 4u, 2u, 5u, 1u, 0x0b274242u,
         {0x3f82cf61u, 0x3f8072e9u, 0x3f80ffffu, 0x3f80fff8u, 0x3f802579u, 0x3f80e5adu,
          0x3f807fffu, 0x3f80ffffu}},
        {"rcbig-3-3-3 (carry nibble 7)", 3u, 3u, 3u, 1u, 0x0b27424au,
         {0x3f81cf61u, 0x3f8072e9u, 0x3f800000u, 0x3f800000u, 0x3f802579u, 0x3f80e5adu,
          0x3f808000u, 0x3f800000u}},
        {"rcbig-2-7-1", 2u, 7u, 1u, 1u, 0xde7ede27u,
         {0x3f81cf61u, 0x3f8072e9u, 0x3f80cdefu, 0x3f809bddu, 0x3f802579u, 0x3f80e5adu,
          0x3f8089abu, 0x3f80d357u}},
    };
    /* Six draws in a row from the measured thread-entry RCX (sr_vfpu_thread_ctrl_init),
     * the same rule applied again to each state it leaves. */
    static const uint32_t seq[6] = {0x00094e24u, 0x245a1029u, 0xecf210c2u,
                                    0x91abc47bu, 0xcad33038u, 0x149d0e36u};
    static const uint32_t seq_rcx[8] = {0x3f8094b3u, 0x3f807323u, 0x3f8002a4u, 0x3f800660u,
                                        0x3f80e3ccu, 0x3f8030d0u, 0x3f800000u, 0x3f800000u};
    static const char *const forms[] = {"vrndi-s-issues", "vrndi-s-s323"};
    int bad = 0;
    for (size_t f = 0; f < sizeof(forms) / sizeof(forms[0]); f++) {
        const int c = find_overlap_diff_case(forms[f]);
        CHECKB(bad, c >= 0, "vrndi.s word is in the overlap-diff header (AOT emits it)");
        const uint32_t w = c >= 0 ? overlap_diff_cases[c].w : (f == 0 ? 0xd0210000u : 0xd021006eu);
        uint8_t di[1];
        vreg_idx((int)(w & 0x7fu), 1, di);
        for (size_t p = 0; p < sizeof(cells) / sizeof(cells[0]); p++) {
            CpuState before, want;
            const uint32_t cs[4] = {0x2468ace0u, 0x13579bdfu, c2s[cells[p].i], c3s[cells[p].j]};
            setup_identity_compute(&before);
            for (int i = 0; i < 128; i++) before.vi[i] = 0x3f800000u | ((uint32_t)i << 16);
            before.vfpuCtrl[3] = 0x5au; /* VFPU_CC preset: vrndi must leave it */
            vrndi_probe_image(&before.vfpuCtrl[8], cs, carries[cells[p].big][cells[p].k]);
            want = before;
            want.vi[di[0]] = cells[p].value;
            memcpy(&want.vfpuCtrl[8], cells[p].after, sizeof(cells[p].after));
            bad |= vrndi_lanes_match(c, w, di[0], &before, &want, cells[p].name);
        }
        CpuState aot, interp;
        setup_identity_compute(&interp);
        sr_vfpu_thread_ctrl_init(&interp);
        aot = interp;
        int seq_ok = 1;
        for (int d = 0; d < 6; d++) {
            seq_ok &= sr_vfpu_interp(&interp, w) == SR_VFPU_COMPUTE && interp.vi[di[0]] == seq[d];
            if (c >= 0) {
                overlap_diff_run_codegen(&aot, c);
                seq_ok &= aot.vi[di[0]] == seq[d];
            }
        }
        seq_ok &= memcmp(&interp.vfpuCtrl[8], seq_rcx, sizeof(seq_rcx)) == 0;
        if (c >= 0) seq_ok &= memcmp(&aot, &interp, sizeof(aot)) == 0;
        CHECKB(bad, seq_ok, "six vrndi.s draws from the thread-entry RCX follow the rule in both lanes");
    }
    /* Outside the measured domain the interpreter refuses with no state change (the AOT
     * body stops the run on the same prefix test; tools/test_codegen_vfpu_fallback.py
     * executes that branch): a pending S, T or D prefix, the .p/.t/.q widths, and the
     * form with no recorded rule (vrnds.s), and vrndf1.s/vrndf2.s under a prefix
     * (their identity-prefix draws are check_vfpu_forms_measured's). */
    static const struct { uint32_t w, pfx[3]; } refused[] = {
        {0xd0210000u, {0xf00e4u, 0xe4u, 0u}},    /* vrndi.s, S prefix pending */
        {0xd0210000u, {0xe4u, 0x1bu, 0u}},       /* vrndi.s, T prefix pending */
        {0xd0210000u, {0xe4u, 0xe4u, 0x100u}},   /* vrndi.s, D write mask on lane 0 */
        {0xd0210000u, {0xe4u, 0xe4u, 0x1u}},     /* vrndi.s, D saturation [0, 1] */
        {0xd0210080u, {0xe4u, 0xe4u, 0u}},       /* vrndi.p */
        {0xd0218000u, {0xe4u, 0xe4u, 0u}},       /* vrndi.t */
        {0xd0218080u, {0xe4u, 0xe4u, 0u}},       /* vrndi.q */
        {0xd0200000u, {0xe4u, 0xe4u, 0u}},       /* vrnds.s */
        {0xd0220000u, {0xf00e4u, 0xe4u, 0u}},    /* vrndf1.s, S prefix pending */
        {0xd0230000u, {0xe4u, 0x1bu, 0u}},       /* vrndf2.s, T prefix pending */
    };
    for (size_t r = 0; r < sizeof(refused) / sizeof(refused[0]); r++) {
        CpuState s;
        setup_identity_compute(&s);
        sr_vfpu_thread_ctrl_init(&s);
        for (int i = 0; i < 3; i++) s.vfpuCtrl[i] = refused[r].pfx[i];
        const CpuState before = s;
        const int kind = sr_vfpu_interp(&s, refused[r].w);
        CHECKB(bad, kind == SR_VFPU_OTHER && memcmp(&s, &before, sizeof(s)) == 0,
               "unmeasured vrnd form or prefix fails closed with no state change");
    }
    return bad;
}

/* Run one PSP-VFPU-FORMS-001 word in both lanes from the probe's image (word i =
 * 0x3f800000 | i << 16, VFPU_CC 0x15, identity prefixes) with the source lanes at image
 * 0 and 1 (C000 / S000) and, when rcx is given, RCX0-7 written. Both lanes must end with
 * exactly want_n destination words at image dest.. (the measured ones) and every other
 * register and control word unchanged, except RCX0-7 = rcx_after for a draw. */
static int forms_cell(const char *form, uint32_t a, uint32_t b, const uint32_t *rcx,
                      const uint32_t *want, int want_n, int dest, const uint32_t *rcx_after) {
    const int c = find_overlap_diff_case(form);
    CpuState before, expect, aot, interp;
    setup_identity_compute(&before);
    for (int i = 0; i < 128; i++) before.vi[i] = 0x3f800000u | ((uint32_t)i << 16);
    before.vi[0] = a;
    before.vi[1] = b;
    before.vfpuCtrl[3] = 0x15u;
    if (rcx) memcpy(&before.vfpuCtrl[8], rcx, 8 * sizeof(uint32_t));
    else sr_vfpu_thread_ctrl_init(&before);
    expect = before;
    for (int i = 0; i < want_n; i++) expect.vi[dest + i] = want[i];
    if (rcx_after) memcpy(&expect.vfpuCtrl[8], rcx_after, 8 * sizeof(uint32_t));
    aot = interp = before;
    const int kind = c >= 0 ? sr_vfpu_interp(&interp, overlap_diff_cases[c].w) : -1;
    if (c >= 0) overlap_diff_run_codegen(&aot, c);
    const int ok = c >= 0 && kind == SR_VFPU_COMPUTE &&
                   memcmp(&interp, &expect, sizeof(expect)) == 0 &&
                   memcmp(&aot, &expect, sizeof(expect)) == 0;
    g_checks++;
    if (!ok) {
        g_failures++;
        fprintf(stderr, "FAIL: PSP-VFPU-FORMS-001 %s in=0x%08x,0x%08x kind=%d lane0 psp=0x%08x "
                "aot=0x%08x interp=0x%08x\n", form, a, b, kind, want[0], aot.vi[dest],
                interp.vi[dest]);
    }
    return !ok;
}

static int check_vfpu_forms_measured(void) {
    /* PSP-VFPU-FORMS-001 (PSP-3000, 6.61, 2026-10-28, probe case vfpu-forms, issue #326;
     * docs/HARDWARE_ORACLE.md): the no-prefix result words of the probe's own words,
     * copied from its stream (both passes were identical). Before this change the AOT
     * raised Unsupported for every one of these forms and the interpreter answered
     * SR_VFPU_OTHER, so each cell failed. Each runs the AOT body tools/codegen.py emits
     * (via the overlap-diff header) and the production interpreter. */
    /* vh2f.s C200, S000: the source word (low half first) and C200 lanes 0-1. */
    static const uint32_t h2f_s[12][3] = {
        {0x80000000u, 0x00000000u, 0x80000000u}, {0x80010001u, 0x00000000u, 0x80000000u},
        {0x020003ffu, 0x00000000u, 0x00000000u}, {0x3c000400u, 0x38800000u, 0x3f800000u},
        {0xc0007bffu, 0x477fe000u, 0xc0000000u}, {0xfc007c00u, 0x7f800000u, 0xff800000u},
        {0x7e007c01u, 0x7f800001u, 0x7f800200u}, {0xfe007fffu, 0x7f8003ffu, 0xff800200u},
        {0xb7ff3555u, 0x3eaaa000u, 0xbeffe000u}, {0x12345640u, 0x42c80000u, 0x3a468000u},
        {0xfbff8400u, 0xb8800000u, 0xc77fe000u}, {0xfdff83ffu, 0x80000000u, 0xff8001ffu},
    };
    /* vh2f.p C200, C000 (and in place, C000, C000): two source words, four lanes. */
    static const uint32_t h2f_p[6][6] = {
        {0x80000000u, 0x80010001u, 0x00000000u, 0x80000000u, 0x00000000u, 0x80000000u},
        {0x020003ffu, 0x3c000400u, 0x00000000u, 0x00000000u, 0x38800000u, 0x3f800000u},
        {0xc0007bffu, 0xfc007c00u, 0x477fe000u, 0xc0000000u, 0x7f800000u, 0xff800000u},
        {0x7e007c01u, 0xfe007fffu, 0x7f800001u, 0x7f800200u, 0x7f8003ffu, 0xff800200u},
        {0xb7ff3555u, 0x12345640u, 0x3eaaa000u, 0xbeffe000u, 0x42c80000u, 0x3a468000u},
        {0xfbff8400u, 0xfdff83ffu, 0xb8800000u, 0xc77fe000u, 0x80000000u, 0xff8001ffu},
    };
    /* vuc2i.s C200, S000: the source word and C200 lanes 0-3. */
    static const uint32_t uc2i[10][5] = {
        {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
        {0xffffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu},
        {0x7f80ff01u, 0x00808080u, 0x7fffffffu, 0x40404040u, 0x3fbfbfbfu},
        {0x80402010u, 0x08080808u, 0x10101010u, 0x20202020u, 0x40404040u},
        {0x01020304u, 0x02020202u, 0x01818181u, 0x01010101u, 0x00808080u},
        {0xfedcba98u, 0x4c4c4c4cu, 0x5d5d5d5du, 0x6e6e6e6eu, 0x7f7f7f7fu},
        {0x3f800000u, 0x00000000u, 0x00000000u, 0x40404040u, 0x1f9f9f9fu},
        {0x7fc00000u, 0x00000000u, 0x00000000u, 0x60606060u, 0x3fbfbfbfu},
        {0x00000001u, 0x00808080u, 0x00000000u, 0x00000000u, 0x00000000u},
        {0x00ff00ffu, 0x7fffffffu, 0x00000000u, 0x7fffffffu, 0x00000000u},
    };
    /* vsocp.s C200, S000: the operand and C200 lanes 0-1. */
    static const uint32_t socp_s[22][3] = {
        {0x00000000u, 0x3f800000u, 0x00000000u}, {0x80000000u, 0x3f800000u, 0x00000000u},
        {0x3e800000u, 0x3f400000u, 0x3e800000u}, {0x3f000000u, 0x3f000000u, 0x3f000000u},
        {0x3f800000u, 0x00000000u, 0x3f800000u}, {0x3fc00000u, 0x00000000u, 0x3f800000u},
        {0x40000000u, 0x00000000u, 0x3f800000u}, {0xbf000000u, 0x3f800000u, 0x00000000u},
        {0xbf800000u, 0x3f800000u, 0x00000000u}, {0xc0000000u, 0x3f800000u, 0x00000000u},
        {0x7f800000u, 0x00000000u, 0x3f800000u}, {0xff800000u, 0x3f800000u, 0x00000000u},
        {0x7fc00000u, 0x7f800001u, 0x7f800001u}, {0xffc00000u, 0x7f800001u, 0x7f800001u},
        {0x7f800001u, 0x7f800001u, 0x7f800001u}, {0x00000001u, 0x3f800000u, 0x00000000u},
        {0x80000001u, 0x3f800000u, 0x00000000u}, {0x3f7fffffu, 0x33800000u, 0x3f7fffffu},
        {0x3f800001u, 0x00000000u, 0x3f800000u}, {0x33800000u, 0x3f7fffffu, 0x33800000u},
        {0xb3800000u, 0x3f800000u, 0x00000000u}, {0x3f400000u, 0x3e800000u, 0x3f400000u},
    };
    /* vsocp.p C200, C000: two operands and C200 lanes 0-3. */
    static const uint32_t socp_p[11][6] = {
        {0x00000000u, 0x80000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u},
        {0x3e800000u, 0x3f000000u, 0x3f400000u, 0x3e800000u, 0x3f000000u, 0x3f000000u},
        {0x3f800000u, 0x3fc00000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u},
        {0x40000000u, 0xbf000000u, 0x00000000u, 0x3f800000u, 0x3f800000u, 0x00000000u},
        {0xbf800000u, 0xc0000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u},
        {0x7f800000u, 0xff800000u, 0x00000000u, 0x3f800000u, 0x3f800000u, 0x00000000u},
        {0x7fc00000u, 0xffc00000u, 0x7f800001u, 0x7f800001u, 0x7f800001u, 0x7f800001u},
        {0x7f800001u, 0x00000001u, 0x7f800001u, 0x7f800001u, 0x3f800000u, 0x00000000u},
        {0x80000001u, 0x3f7fffffu, 0x3f800000u, 0x00000000u, 0x33800000u, 0x3f7fffffu},
        {0x3f800001u, 0x33800000u, 0x00000000u, 0x3f800000u, 0x3f7fffffu, 0x33800000u},
        {0xb3800000u, 0x3f400000u, 0x3f800000u, 0x00000000u, 0x3e800000u, 0x3f400000u},
    };
    /* vwbn.s S200, S000, e: the 28 operands, then the result word at each exponent. */
    static const uint32_t wbn_in[28] = {
        0x00000000u, 0x80000000u, 0x3f800000u, 0x3fc00000u, 0x40000000u, 0x40400000u,
        0x40b00000u, 0x3f000000u, 0x3f400000u, 0x3e800000u, 0x2edbe6ffu, 0x501502f9u,
        0x4b000000u, 0x4b800001u, 0x4e800000u, 0xc0500000u, 0xbe99999au, 0x7f7fffffu,
        0x00800000u, 0x00400000u, 0x00000001u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
        0x7f800001u, 0x40490fdbu, 0x12345678u, 0x8abcdef0u,
    };
    static const uint32_t wbn_e[8] = {0x00u, 0x01u, 0x7eu, 0x7fu, 0x80u, 0x96u, 0xfeu, 0xffu};
    static const uint32_t wbn_out[8][28] = {
        {
            0x00000000u, 0x80000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00400000u,
            0x00600000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x005fe000u, 0x001502f9u,
            0x00000000u, 0x00000080u, 0x00000000u, 0x80500000u, 0x80334000u, 0x007fc000u,
            0x00000000u, 0x00400000u, 0x00000001u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
            0x7f800001u, 0x00490fdbu, 0x00456780u, 0x801bde00u,
        },
        {
            0x00800000u, 0x80800000u, 0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u,
            0x00b00000u, 0x00800000u, 0x00800000u, 0x00800000u, 0x00eff000u, 0x00fc8000u,
            0x00800000u, 0x00800040u, 0x00800000u, 0x80800000u, 0x8099a000u, 0x00ffe000u,
            0x00800000u, 0x00c00000u, 0x00800001u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
            0x7f800001u, 0x00ed8000u, 0x00a2b3c0u, 0x80cdef00u,
        },
        {
            0x3f000000u, 0xbf000000u, 0x3f000000u, 0x3f000000u, 0x3f000000u, 0x3f000000u,
            0x3f000000u, 0x3f000000u, 0x3f400000u, 0x3f400000u, 0x3f6df37fu, 0x3f540be4u,
            0x3f000000u, 0x3f000200u, 0x3f000000u, 0xbf400000u, 0xbf4ccccdu, 0x3f7fffffu,
            0x3f000400u, 0x3f400000u, 0x3f000001u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
            0x7f800001u, 0x3f243f6cu, 0x3f002d15u, 0xbf005e6fu,
        },
        {
            0x3f800000u, 0xbf800000u, 0x3f800000u, 0x3fc00000u, 0x3f800000u, 0x3f800000u,
            0x3fc00000u, 0x3fc00000u, 0x3fe00000u, 0x3fa00000u, 0x3fb6f9bfu, 0x3faa05f2u,
            0x3f800000u, 0x3f800100u, 0x3f800000u, 0xbfa00000u, 0xbfa66666u, 0x3fff8000u,
            0x3f800200u, 0x3fc00000u, 0x3f800001u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
            0x7f800001u, 0x3f921fb6u, 0x3f80168au, 0xbf802f37u,
        },
        {
            0x40000000u, 0xc0000000u, 0x40400000u, 0x40600000u, 0x40000000u, 0x40400000u,
            0x40600000u, 0x40200000u, 0x40300000u, 0x40100000u, 0x401b7cdfu, 0x401502f9u,
            0x40000000u, 0x40000080u, 0x40000000u, 0xc0500000u, 0xc0133333u, 0x407fc000u,
            0x40000100u, 0x40400000u, 0x40000001u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
            0x7f800001u, 0x40490fdbu, 0x40000b45u, 0xc000179bu,
        },
        {
            0x4b000000u, 0xcb000000u, 0x4b010000u, 0x4b018000u, 0x4b020000u, 0x4b030000u,
            0x4b058000u, 0x4b008000u, 0x4b00c000u, 0x4b004000u, 0x4b006df3u, 0x4b0be400u,
            0x4b000000u, 0x4b000002u, 0x4b000000u, 0xcb034000u, 0xcb004cccu, 0x4b7fff00u,
            0x4b040000u, 0x4b400000u, 0x4b000001u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
            0x7f800001u, 0x4b03243fu, 0x4b2d159eu, 0xcb5e6f78u,
        },
        {
            0x7f000000u, 0xff000000u, 0x7f000100u, 0x7f000180u, 0x7f000200u, 0x7f000300u,
            0x7f000580u, 0x7f000000u, 0x7f400000u, 0x7f400000u, 0x7f6df37fu, 0x7f000254u,
            0x7f008000u, 0x7f010000u, 0x7f400000u, 0xff000340u, 0xff4ccccdu, 0x7f7fffffu,
            0x7f000400u, 0x7f400000u, 0x7f000001u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
            0x7f800001u, 0x7f000324u, 0x7f002d15u, 0xff005e6fu,
        },
        {
            0x7f800000u, 0xff800000u, 0x7f800000u, 0x7fc00000u, 0x7f800100u, 0x7f800180u,
            0x7f8002c0u, 0x7fc00000u, 0x7fe00000u, 0x7fa00000u, 0x7fb6f9bfu, 0x7f80012au,
            0x7f804000u, 0x7f808000u, 0x7fa00000u, 0xff8001a0u, 0xffa66666u, 0x7fffffffu,
            0x7f800200u, 0x7fc00000u, 0x7f800001u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
            0x7f800001u, 0x7f800192u, 0x7f80168au, 0xff802f37u,
        },
    };
    /* vrndf1.s / vrndf2.s S000 from 17 written RCX states: RCX0-7 written, the vrndf1
     * and vrndf2 words, and RCX0-7 after the draw (the same for both forms). */
    static const uint32_t rndf[17][18] = {
        {0x3f80ace0u, 0x3f809bdfu, 0x3f800000u, 0x3f800000u, 0x3f802468u, 0x3f801357u, 0x3f800000u, 0x3f800000u,
         0x3fa7424au, 0x4027424au, 0x3f80cf61u, 0x3f8072e9u, 0x3f800000u, 0x3f800000u, 0x3f802579u, 0x3f80e5adu, 0x3f800000u, 0x3f800000u},
        {0x3f81bdf1u, 0x3f809bdfu, 0x3f800001u, 0x3f800000u, 0x3f803579u, 0x3f801347u, 0x3f800000u, 0x3f808000u,
         0x3fd0cae1u, 0x4050cae1u, 0x3f8056feu, 0x3f8073e1u, 0x3f800000u, 0x3f800002u, 0x3f80bf13u, 0x3f80e7bdu, 0x3f808000u, 0x3f800000u},
        {0x3f82cf02u, 0x3f809bdfu, 0x3f80ffffu, 0x3f80e6d8u, 0x3f80468au, 0x3f801377u, 0x3f807fffu, 0x3f80c4d5u,
         0x3fe61d45u, 0x40661d45u, 0x3f80de9bu, 0x3f8070f9u, 0x3f80e6d8u, 0x3f80cdb1u, 0x3f8058acu, 0x3f80e18du, 0x3f80c4d5u, 0x3f8009abu},
        {0x3f83e013u, 0x3f809bdfu, 0x3f800000u, 0x3f800001u, 0x3f80579bu, 0x3f801367u, 0x3f808000u, 0x3f800000u,
         0x3fe3d82eu, 0x4063d82eu, 0x3f806638u, 0x3f8071f1u, 0x3f800001u, 0x3f800005u, 0x3f80f246u, 0x3f80e39du, 0x3f800000u, 0x3f808000u},
        {0x3f8ff124u, 0x3f809bdfu, 0x3f80fffeu, 0x3f800000u, 0x3f8068acu, 0x3f801317u, 0x3f80ffffu, 0x3f80c000u,
         0x3fcd64abu, 0x404d64abu, 0x3f81edd5u, 0x3f8076c9u, 0x3f800000u, 0x3f80000du, 0x3f808bdfu, 0x3f80ededu, 0x3f80c000u, 0x3f808000u},
        {0x3f800235u, 0x3f819bdfu, 0x3f80ffffu, 0x3f80cdefu, 0x3f8079beu, 0x3f801307u, 0x3f80ffffu, 0x3f8089abu,
         0x3fce8920u, 0x404e8920u, 0x3f817572u, 0x3f8077c1u, 0x3f80cdefu, 0x3f809bedu, 0x3f802579u, 0x3f80effdu, 0x3f8089abu, 0x3f801357u},
        {0x3f8f1346u, 0x3f8f9bdfu, 0x3f8f6e50u, 0x3f8fffffu, 0x3f808acfu, 0x3f801337u, 0x3f80907fu, 0x3f807fffu,
         0x3fe0e035u, 0x4060e035u, 0x3f80fd0fu, 0x3f8074d9u, 0x3f80ffffu, 0x3f806e4du, 0x3f80bf12u, 0x3f80e9cdu, 0x3f807fffu, 0x3f809080u},
        {0x3f882457u, 0x3f879bdfu, 0x3f865a5au, 0x3f85ffffu, 0x3f849be0u, 0x3f831327u, 0x3f825a5au, 0x3f81ffffu,
         0x3f98ab4du, 0x4018ab4du, 0x3f8084acu, 0x3f8075d1u, 0x3f80ffffu, 0x3f80b0d0u, 0x3f8058acu, 0x3f80ebddu, 0x3f80ffffu, 0x3f806c8eu},
        {0x3f803568u, 0x3f809bdfu, 0x3f800000u, 0x3f800000u, 0x3f80acf1u, 0x3f8013d7u, 0x3f800000u, 0x3f800000u,
         0x3ff386f2u, 0x407386f2u, 0x3f800c49u, 0x3f807aa9u, 0x3f800000u, 0x3f800000u, 0x3f80f246u, 0x3f80f52du, 0x3f800000u, 0x3f800000u},
        {0x3f814679u, 0x3f809bdfu, 0x3f800001u, 0x3f800000u, 0x3f80be02u, 0x3f8013c7u, 0x3f800000u, 0x3f808000u,
         0x3f9d0f89u, 0x401d0f89u, 0x3f8093e6u, 0x3f807ba1u, 0x3f800000u, 0x3f800002u, 0x3f808bdfu, 0x3f80f73du, 0x3f808000u, 0x3f800000u},
        {0x3f82578au, 0x3f809bdfu, 0x3f80ffffu, 0x3f80e6d8u, 0x3f80cf13u, 0x3f8013f7u, 0x3f807fffu, 0x3f80c4d5u,
         0x3fb261edu, 0x403261edu, 0x3f801b83u, 0x3f8078b9u, 0x3f80e6d8u, 0x3f80cdb1u, 0x3f802579u, 0x3f80f10du, 0x3f80c4d5u, 0x3f8009abu},
        {0x3f83689bu, 0x3f809bdfu, 0x3f800000u, 0x3f800001u, 0x3f80e024u, 0x3f8013e7u, 0x3f808000u, 0x3f800000u,
         0x3fb01cd6u, 0x40301cd6u, 0x3f80a320u, 0x3f8079b1u, 0x3f800001u, 0x3f800005u, 0x3f80bf12u, 0x3f80f31du, 0x3f800000u, 0x3f808000u},
        {0x3f8f79acu, 0x3f809bdfu, 0x3f80fffeu, 0x3f800000u, 0x3f80f135u, 0x3f801397u, 0x3f80ffffu, 0x3f80c000u,
         0x3f99a953u, 0x4019a953u, 0x3f812abdu, 0x3f807e89u, 0x3f800000u, 0x3f80000du, 0x3f8058acu, 0x3f80fd6du, 0x3f80c000u, 0x3f808000u},
        {0x3f808abdu, 0x3f819bdfu, 0x3f80ffffu, 0x3f80cdefu, 0x3f800246u, 0x3f801387u, 0x3f80ffffu, 0x3f8089abu,
         0x3f9acdc8u, 0x401acdc8u, 0x3f81b25au, 0x3f807f81u, 0x3f80cdefu, 0x3f809bedu, 0x3f80f245u, 0x3f80ff7du, 0x3f8089abu, 0x3f801357u},
        {0x3f8f9bceu, 0x3f8f9bdfu, 0x3f8f6e50u, 0x3f8fffffu, 0x3f801357u, 0x3f8013b7u, 0x3f80907fu, 0x3f807fffu,
         0x3fad24ddu, 0x402d24ddu, 0x3f8039f7u, 0x3f807c99u, 0x3f80ffffu, 0x3f806e4du, 0x3f808bdfu, 0x3f80f94du, 0x3f807fffu, 0x3f809080u},
        {0x3f88acdfu, 0x3f879bdfu, 0x3f865a5au, 0x3f85ffffu, 0x3f842468u, 0x3f8313a7u, 0x3f825a5au, 0x3f81ffffu,
         0x3fe4eff5u, 0x4064eff5u, 0x3f80c194u, 0x3f807d91u, 0x3f80ffffu, 0x3f80b0d0u, 0x3f802578u, 0x3f80fb5du, 0x3f80ffffu, 0x3f806c8eu},
        {0x3f800001u, 0x3f800002u, 0x3f800004u, 0x3f800008u, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
         0x3f894e24u, 0x40094e24u, 0x3f800dceu, 0x3f804042u, 0x3f800008u, 0x3f800014u, 0x3f800001u, 0x3f800008u, 0x3f800000u, 0x3f800000u},
    };
    static const char *const wbn_forms[8] = {"wbn-s-00", "wbn-s-01", "wbn-s-7e", "wbn-s-7f",
                                             "wbn-s-80", "wbn-s-96", "wbn-s-fe", "wbn-s-ff"};
    int bad = 0;
    for (int i = 0; i < 12; i++)
        bad |= forms_cell("h2f-s-forms", h2f_s[i][0], 0x7a5a5a5au, NULL, &h2f_s[i][1], 2, 32, NULL);
    for (int i = 0; i < 6; i++) {
        bad |= forms_cell("h2f-p-forms", h2f_p[i][0], h2f_p[i][1], NULL, &h2f_p[i][2], 4, 32, NULL);
        bad |= forms_cell("h2f-p-inplace", h2f_p[i][0], h2f_p[i][1], NULL, &h2f_p[i][2], 4, 0, NULL);
    }
    for (int i = 0; i < 10; i++)
        bad |= forms_cell("uc2i-s-forms", uc2i[i][0], 0x7a5a5a5au, NULL, &uc2i[i][1], 4, 32, NULL);
    for (int i = 0; i < 22; i++)
        bad |= forms_cell("socp-s-forms", socp_s[i][0], 0x7a5a5a5au, NULL, &socp_s[i][1], 2, 32, NULL);
    for (int i = 0; i < 11; i++)
        bad |= forms_cell("socp-p-forms", socp_p[i][0], socp_p[i][1], NULL, &socp_p[i][2], 4, 32, NULL);
    for (int e = 0; e < 8; e++) {
        const int c = find_overlap_diff_case(wbn_forms[e]);
        CHECKB(bad, c >= 0 && ((overlap_diff_cases[c].w >> 16) & 0xffu) == wbn_e[e],
               "vwbn.s probe word carries its exponent");
        for (int i = 0; i < 28; i++)
            bad |= forms_cell(wbn_forms[e], wbn_in[i], 0x7a5a5a5au, NULL, &wbn_out[e][i], 1, 32, NULL);
    }
    for (int k = 0; k < 17; k++) {
        bad |= forms_cell("rndf1-s-forms", 0x7a5a5a5au, 0x7a5a5a5au, rndf[k], &rndf[k][8], 1, 0,
                          &rndf[k][10]);
        bad |= forms_cell("rndf2-s-forms", 0x7a5a5a5au, 0x7a5a5a5au, rndf[k], &rndf[k][9], 1, 0,
                          &rndf[k][10]);
    }
    /* Outside the measured domain the interpreter refuses with no state change (the AOT
     * body stops the run on the same prefix test; tools/test_codegen_vfpu_fallback.py
     * executes that branch): a pending S, T or D prefix, and the widths no cell covered. */
    static const struct { uint32_t w, pfx[3]; } refused[] = {
        {0xd0330088u, {0xf00e4u, 0xe4u, 0u}},   /* vh2f.p, S prefix (negate-all) pending */
        {0xd0330088u, {0xe4u, 0x1bu, 0u}},      /* vh2f.p, T prefix pending */
        {0xd0330088u, {0xe4u, 0xe4u, 0x1u}},    /* vh2f.p, D saturation */
        {0xd0338008u, {0xe4u, 0xe4u, 0u}},      /* vh2f.t */
        {0xd0338088u, {0xe4u, 0xe4u, 0u}},      /* vh2f.q */
        {0xd0380008u, {0x5001bu, 0xe4u, 0u}}, /* vuc2i.s, S prefix pending */
        {0xd0380088u, {0xe4u, 0xe4u, 0u}},      /* vuc2i.p */
        {0xd0450008u, {0xe4u, 0xe4u, 0x100u}},  /* vsocp.s, D write mask */
        {0xd0458008u, {0xe4u, 0xe4u, 0u}},      /* vsocp.t */
        {0xd37f0008u, {0xf00e4u, 0xe4u, 0u}},   /* vwbn.s, S prefix pending */
        {0xd37f0088u, {0xe4u, 0xe4u, 0u}},      /* vwbn.p */
        {0xd3e08080u, {0xe4u, 0xe4u, 0u}},      /* vwbn.q */
        {0xd0220000u, {0xe4u, 0xe4u, 0x3u}},    /* vrndf1.s, D saturation */
        {0xd0220080u, {0xe4u, 0xe4u, 0u}},      /* vrndf1.p */
        {0xd0238080u, {0xe4u, 0xe4u, 0u}},      /* vrndf2.q */
    };
    for (size_t r = 0; r < sizeof(refused) / sizeof(refused[0]); r++) {
        CpuState s;
        setup_identity_compute(&s);
        sr_vfpu_thread_ctrl_init(&s);
        for (int i = 0; i < 128; i++) s.vi[i] = 0x3f800000u | ((uint32_t)i << 16);
        for (int i = 0; i < 3; i++) s.vfpuCtrl[i] = refused[r].pfx[i];
        const CpuState before = s;
        const int kind = sr_vfpu_interp(&s, refused[r].w);
        CHECKB(bad, kind == SR_VFPU_OTHER && memcmp(&s, &before, sizeof(s)) == 0,
               "unmeasured vfpu-forms width or prefix fails closed with no state change");
    }
    return bad;
}

int main(void) {
    int bad = 0;
    bad |= check_psp_measured_vfpu9();
    bad |= check_vrndi_rule_cells();
    bad |= check_vfpu_forms_measured();
    bad |= check_cop2_control_transfers();
    bad |= check_quad_memops();
    bad |= check_vcrs_widths();
    bad |= check_vasin_measured_domain_words();
    bad |= check_vfpu_compare_measured_words();
    bad |= check_vscmp_hle_issues_cells();
    bad |= check_vrexp2_measured_words();
    bad |= check_reserved_cmov_form();
    bad |= check_vfpu3_compare_forms();
    bad |= check_mfvc_zero_is_noop();
    bad |= check_vrot_overlap();
    bad |= check_vhdp_vmscl_overlap();
    bad |= check_overlap_diff();
    bad |= check_vfpu3_compare_parity();
    bad |= check_vhdp_signed_zero_corpus();
    bad |= check_vf2i_conversions();
    bad |= g_failures != 0;
    if (bad == 0) {
        printf("vfpu_interp_selftest: all %d checks passed\n", g_checks);
        return 0;
    }
    fprintf(stderr, "vfpu_interp_selftest: %d/%d checks FAILED\n", g_failures, g_checks);
    return 1;
}
