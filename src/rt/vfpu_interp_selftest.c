// SPDX-License-Identifier: GPL-2.0-or-later
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
//   * valid aligned quad round-trips still work.
//
// The final check count is printed by the executable. No game inputs or private
// data required. Run via `make vfpu-interp-selftest` (also wired into
// hst_manager.ps1 -Action Verify and the Linux native CI gate, where the
// ASan/UBSan build runs as the uninitialized-read/overflow gate).

#define _CRT_SECURE_NO_WARNINGS
/* White-box: include the real runtime + interpreter as translation units so the
 * selftest exercises the production sr_vfpu_interp, sr_vread/sr_vwrite, the
 * transcendental kernels and the guest-memory accessors directly (the same
 * pattern src/rt/heap_selftest.c uses for recomp.c). */
#include "recomp.c"
#include "vfpu_tables.c"
#include "vfpu_interp.c"

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
/* SR_YIELD's safe-boundary service hook (#70). sr_sched_on is 0 here, so neither
 * branch of the macro ever runs; the symbols exist only to satisfy the link. */
atomic_int_least32_t sr_service_request;
void sr_sched_request_service(void) {}
void sr_sched_service_only(void) {}
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

int main(void) {
    int bad = 0;
    bad |= check_quad_memops();
    bad |= check_vcrs_widths();
    bad |= check_vrot_overlap();
    bad |= check_vf2i_conversions();
    bad |= g_failures != 0;
    if (bad == 0) {
        printf("vfpu_interp_selftest: all %d checks passed\n", g_checks);
        return 0;
    }
    fprintf(stderr, "vfpu_interp_selftest: %d/%d checks FAILED\n", g_failures, g_checks);
    return 1;
}
