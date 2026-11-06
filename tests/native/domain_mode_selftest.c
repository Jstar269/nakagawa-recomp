// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// LLE Phase 1 (PR 3) domain-mode selftest: the per-domain HLE/LLE table and
// the sr_import_call() seam (spec section 4). Standalone host executable, no
// game inputs required:
//
//   mingw32-make CC=gcc domain-mode-selftest
//
// The harness #includes recomp.c, cpu_lle.c, and domain_mode.c for direct
// access to the same helpers the generated stubs call, and links the real
// guest_interp.c so the LLE dispatch lane is exercised, not modeled. The stub
// block below mirrors tests/native/cpu_lle_selftest.c: symbols recomp.c references
// that live in scheduler/HLE translation units we deliberately do not link.
// sr_syscall is a recording test double (not the HLE handler): the HLE-arm
// proof is that the seam tail-calls sr_syscall with identical (s, nid) and
// returns its value untouched, which the double observes exactly; the real
// unknown-NID fatal policy behind that call is owned by hle.c and pinned by
// tools/test_dispatch_fatal_policy.py, unchanged by this PR.
//
// Failing-before evidence (PR 3, spec section 11): on the base tree there is
// no domain_mode.h/.c, generated imports call sr_syscall directly, and no
// per-domain selection exists, so every assertion here fails to build.
//
// Expected stderr noise: DISPATCH_MISS_NEW / INTERP_REJECT lines from the
// deliberate dispatch-rejection cases, SR_IMPORT_FATAL lines from the
// fail-closed cases, and one SR_IMPORT_FALLBACK_HLE line. The exit code is
// the result.

#include "recomp.c"       /* arena, dispatch table, dispatch_call_try */
#include "cpu_lle.c"      /* flow enum + helpers the linked interp needs */
#include "domain_mode.c"  /* mode table and seam under test (same TU) */

/* No GE renderer is linked by this domain-mode-only harness. */
/* A renderer stand-in for the COSIM VRAM cells: while g_sr_vram_read_pending is
 * set it holds one byte newer than the host copy, and any read or write
 * preparation reads the render target back (that byte) and clears the flag, as a
 * framebuffer readback does. Commits of whole 4 KiB pages are counted. */
volatile int g_sr_vram_read_pending;
static uint32_t s_vram_truth_addr;     /* guest address of the renderer-held byte */
static uint8_t s_vram_truth_value;
static uint32_t s_vram_readbacks;
static uint32_t s_vram_page_commit_addr;
static uint32_t s_vram_page_commits;
static void vram_readback(void) {
    if (!g_sr_vram_read_pending) return;
    if (s_vram_truth_addr) *(uint8_t *)SR_HOST(s_vram_truth_addr) = s_vram_truth_value;
    g_sr_vram_read_pending = 0;
    s_vram_readbacks++;
}
void sr_ge_vram_read_prepare(uint32_t addr, uint32_t width) { (void)addr; (void)width; vram_readback(); }
void sr_ge_vram_write_prepare(uint32_t addr, uint32_t width) { (void)addr; (void)width; vram_readback(); }
void sr_ge_vram_write_commit(uint32_t addr, uint32_t width) {
    if (width == 4096u) { s_vram_page_commits++; s_vram_page_commit_addr = addr; }
}

#include <stdlib.h>
#include <string.h>

/* ---- stubs for runtime symbols recomp.c references -------------------------------- */

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
void sr_yield(CpuState *s) { (void)s; }
int sr_vfpu_interp(CpuState *s, uint32_t op) { (void)s; (void)op; return 0; }
uint64_t SDL_GetTicksNS(void) { return 0u; }

/* Recording sr_syscall double: observes the exact (s, nid) the seam passes
 * and answers a canned value, so the HLE-arm assertions below prove the seam
 * neither rewrites arguments nor filters the handler result. */
static unsigned s_syscall_calls;
static uint32_t s_syscall_last_nid;
static CpuState *s_syscall_last_s;
static uint32_t s_syscall_canned = 0x1EAF0001u;
/* COSIM fixtures: the HLE stand-in can write one guest byte, records the a0
 * it was given, and can issue one nested seam call (a guest callback). */
static uint32_t s_syscall_last_a0;
static uint32_t s_hle_write_addr;
static uint8_t s_hle_write_value;
static uint32_t s_hle_reenter_nid;
static int s_hle_fail_flow;   /* the HLE stand-in's guest callback fails */

int sr_import_call(CpuState *s, uint32_t nid, uint32_t stub_pc);

uint32_t sr_syscall(CpuState *s, uint32_t nid) {
    s_syscall_calls++;
    s_syscall_last_nid = nid;
    s_syscall_last_s = s;
    s_syscall_last_a0 = s->r[4];
    if (s_hle_write_addr != 0u) {
        MEM_W8(s_hle_write_addr, s_hle_write_value);
    }
    if (s_hle_reenter_nid != 0u && nid != s_hle_reenter_nid) {
        uint32_t inner = s_hle_reenter_nid;
        s_hle_reenter_nid = 0u;
        (void)sr_import_call(s, inner, 0x08813100u);
        s_syscall_last_nid = nid;
    }
    if (s_hle_fail_flow) {
        s->flow_kind = SR_FLOW_FATAL;
        s->flow_target = nid;
    }
    return s_syscall_canned;
}

/* ---- harness ---------------------------------------------------------------------- */

static int g_failed = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL L%d: ", __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); g_failed = 1; } \
} while (0)

#define NID_HLE     0x27A6B7CDu
#define NID_LLE     0x0C6228E4u
#define NID_LLEHIT  0x5A4E8D91u
#define NID_FB      0x3B19D3A9u
#define NID_FBHIT   0x73E3A3ABu
#define NID_COSIM   0x11223344u
#define NID_BAD     0xDEADBEEFu

#define STUB_PC      0x08813000u
#define TEST_EXPORT  0x08812000u
#define UNOWNED_ADDR 0x08820000u

static void fresh_state(CpuState *s) {
    memset(s, 0, sizeof *s);
    sr_domain_reset_defaults();
    s_syscall_calls = 0u;
    s_syscall_last_nid = 0u;
    s_syscall_last_s = NULL;
    s_syscall_last_a0 = 0u;
    s_hle_write_addr = 0u;
    s_hle_write_value = 0u;
    s_hle_reenter_nid = 0u;
    s_hle_fail_flow = 0;
    g_sr_vram_read_pending = 0;
    s_vram_truth_addr = 0u;
    s_vram_readbacks = 0u;
    s_vram_page_commits = 0u;
    s_vram_page_commit_addr = 0u;
    sr_cpu_clear_flow(s);
}

static int s_export_ran;

static void test_export_fn(CpuState *s) {
    s_export_ran++;
    s->r[2] = 0x5EED0001u;
}

/* Every domain defaults to HLE with zeroed accounting and no lock. */
static void test_defaults(void) {
    CpuState s;
    SrDomain d;
    uint32_t out = 0u;
    fresh_state(&s);
    for (d = SR_DOMAIN_CPU; d < SR_DOMAIN_COUNT; d = (SrDomain)(d + 1)) {
        CHECK(sr_domain_mode_get(d) == SR_MODE_HLE, "domain %d must default HLE", (int)d);
    }
    CHECK(!sr_domain_locked(), "table must start unlocked");
    CHECK(sr_domain_fallback_count() == 0u, "fallback count must start 0");
    CHECK(sr_domain_cosim_request_count() == 0u, "cosim count must start 0");
    CHECK(sr_domain_lookup_nid(NID_HLE) == SR_DOMAIN_COUNT, "no NID may be pre-bound");
    CHECK(sr_import_lookup_export(NID_HLE, &out) != 0, "no export may be pre-registered");
}

/* Set/get round-trips every domain and mode; aliases agree; reset restores. */
static void test_set_get_reset(void) {
    CpuState s;
    SrDomain d;
    fresh_state(&s);
    for (d = SR_DOMAIN_CPU; d < SR_DOMAIN_COUNT; d = (SrDomain)(d + 1)) {
        SrDomainMode m;
        for (m = SR_MODE_HLE; m <= SR_MODE_LLE_FALLBACK_HLE; m = (SrDomainMode)(m + 1)) {
            CHECK(sr_domain_mode_set(d, m) == 0, "set(%d,%d) must succeed", (int)d, (int)m);
            CHECK(sr_domain_mode_get(d) == m, "get(%d)=%d, want %d", (int)d,
                  (int)sr_domain_mode_get(d), (int)m);
        }
    }
    CHECK(sr_domain_set_mode(SR_DOMAIN_GE, SR_MODE_LLE) == 0, "alias set must work");
    CHECK(sr_domain_get_mode(SR_DOMAIN_GE) == SR_MODE_LLE, "alias get must agree");
    CHECK(sr_domain_mode_set(SR_DOMAIN_COUNT, SR_MODE_LLE) != 0, "bad domain must fail");
    CHECK(sr_domain_mode_set((SrDomain)-1, SR_MODE_LLE) != 0, "negative domain must fail");
    CHECK(sr_domain_mode_set(SR_DOMAIN_GE, (SrDomainMode)99) != 0, "bad mode must fail");
    CHECK(sr_domain_mode_get(SR_DOMAIN_GE) == SR_MODE_LLE, "failed set must keep prior mode");
    CHECK(sr_domain_mode_get(SR_DOMAIN_COUNT) == SR_MODE_HLE, "bad-domain get must answer HLE");
    sr_domain_reset_defaults();
    for (d = SR_DOMAIN_CPU; d < SR_DOMAIN_COUNT; d = (SrDomain)(d + 1)) {
        CHECK(sr_domain_mode_get(d) == SR_MODE_HLE, "reset must restore HLE on %d", (int)d);
    }
}

/* The lock freezes set/bind/register but never the read path. */
static void test_lock(void) {
    CpuState s;
    fresh_state(&s);
    CHECK(sr_domain_mode_set(SR_DOMAIN_IO, SR_MODE_LLE) == 0, "pre-lock set must succeed");
    CHECK(sr_domain_bind_nid(SR_DOMAIN_IO, NID_FB) == 0, "pre-lock bind must succeed");
    sr_domain_lock();
    CHECK(sr_domain_locked(), "lock flag must read back");
    CHECK(sr_domain_mode_set(SR_DOMAIN_IO, SR_MODE_HLE) != 0, "locked set must fail");
    CHECK(sr_domain_mode_get(SR_DOMAIN_IO) == SR_MODE_LLE, "locked set must keep prior mode");
    CHECK(sr_domain_bind_nid(SR_DOMAIN_IO, NID_HLE) != 0, "locked bind must fail");
    CHECK(sr_import_register_export(NID_HLE, TEST_EXPORT) != 0, "locked register must fail");
    CHECK(sr_domain_mode_get(SR_DOMAIN_IO) == SR_MODE_LLE, "reads still work under lock");
    sr_domain_reset_defaults();
    CHECK(!sr_domain_locked(), "reset must unlock");
    CHECK(sr_domain_mode_get(SR_DOMAIN_IO) == SR_MODE_HLE, "reset must restore HLE");
    CHECK(sr_domain_mode_set(SR_DOMAIN_IO, SR_MODE_LLE) == 0, "set must work after reset");
}

/* Library table: exact names, the scePsmf* family, and HLE-only unknowns. */
static void test_library_table(void) {
    CpuState s;
    fresh_state(&s);
    CHECK(sr_domain_for_library("ThreadManForUser") == SR_DOMAIN_THREADMAN, "ThreadManForUser");
    CHECK(sr_domain_for_library("ThreadManForKernel") == SR_DOMAIN_THREADMAN, "ThreadManForKernel");
    CHECK(sr_domain_for_library("IoFileMgrForUser") == SR_DOMAIN_IO, "IoFileMgrForUser");
    CHECK(sr_domain_for_library("IoFileMgrForKernel") == SR_DOMAIN_IO, "IoFileMgrForKernel");
    CHECK(sr_domain_for_library("sceGe_user") == SR_DOMAIN_GE, "sceGe_user");
    CHECK(sr_domain_for_library("sceAudio") == SR_DOMAIN_AUDIO, "sceAudio");
    CHECK(sr_domain_for_library("sceSasCore") == SR_DOMAIN_AUDIO, "sceSasCore");
    CHECK(sr_domain_for_library("sceAtrac3plus") == SR_DOMAIN_AUDIO, "sceAtrac3plus");
    CHECK(sr_domain_for_library("scePsmf") == SR_DOMAIN_MEDIA, "scePsmf");
    CHECK(sr_domain_for_library("scePsmfPlayer") == SR_DOMAIN_MEDIA, "scePsmfPlayer");
    CHECK(sr_domain_for_library("sceMpeg") == SR_DOMAIN_MEDIA, "sceMpeg");
    CHECK(sr_domain_for_library("sceVideocodec") == SR_DOMAIN_MEDIA, "sceVideocodec");
    CHECK(sr_domain_for_library("InterruptManager") == SR_DOMAIN_INTC, "InterruptManager");
    CHECK(sr_domain_for_library("sceSysTimer") == SR_DOMAIN_TIMER, "sceSysTimer");
    CHECK(sr_domain_for_library("SysTimerForKernel") == SR_DOMAIN_TIMER, "SysTimerForKernel");
    CHECK(sr_domain_for_library(NULL) == SR_DOMAIN_COUNT, "NULL must be unmapped");
    CHECK(sr_domain_for_library("") == SR_DOMAIN_COUNT, "empty must be unmapped");
    CHECK(sr_domain_for_library("ThreadMan") == SR_DOMAIN_COUNT, "prefix must not match");
    CHECK(sr_domain_for_library("sceGe") == SR_DOMAIN_COUNT, "sceGe is not sceGe_user");
    CHECK(sr_domain_for_library("sceKernelThread") == SR_DOMAIN_COUNT, "unknown must be unmapped");
    (void)s;
}

/* NID bindings: idempotent rebind, conflicting rebind fails closed. */
static void test_nid_bindings(void) {
    CpuState s;
    fresh_state(&s);
    CHECK(sr_domain_bind_nid(SR_DOMAIN_THREADMAN, NID_LLE) == 0, "bind must succeed");
    CHECK(sr_domain_lookup_nid(NID_LLE) == SR_DOMAIN_THREADMAN, "lookup must find the bind");
    CHECK(sr_domain_bind_nid(SR_DOMAIN_THREADMAN, NID_LLE) == 0, "same rebind must succeed");
    CHECK(sr_domain_bind_nid(SR_DOMAIN_IO, NID_LLE) != 0, "conflicting rebind must fail");
    CHECK(sr_domain_lookup_nid(NID_LLE) == SR_DOMAIN_THREADMAN, "conflict must keep original");
    CHECK(sr_domain_bind_nid(SR_DOMAIN_COUNT, NID_HLE) != 0, "bad-domain bind must fail");
    CHECK(sr_domain_bind_nid(SR_DOMAIN_IO, 0u) != 0, "zero-NID bind must fail");
    (void)s;
}

/* Export registry: idempotent same-address, conflicting address fails closed. */
static void test_export_registry(void) {
    CpuState s;
    uint32_t out = 0u;
    fresh_state(&s);
    CHECK(sr_import_register_export(NID_LLE, TEST_EXPORT) == 0, "register must succeed");
    CHECK(sr_import_lookup_export(NID_LLE, &out) == 0 && out == TEST_EXPORT,
          "lookup must return 0x%08x (got 0x%08x)", TEST_EXPORT, out);
    CHECK(sr_import_register_export(NID_LLE, TEST_EXPORT) == 0, "same-address re-register must succeed");
    CHECK(sr_import_register_export(NID_LLE, TEST_EXPORT + 4u) != 0, "conflicting address must fail");
    CHECK(sr_import_lookup_export(NID_LLE, &out) == 0 && out == TEST_EXPORT,
          "conflict must keep original (got 0x%08x)", out);
    CHECK(sr_import_register_export(0u, TEST_EXPORT) != 0, "zero NID must fail");
    CHECK(sr_import_register_export(NID_HLE, 0u) != 0, "zero address must fail");
    CHECK(sr_import_register_export(NID_HLE, TEST_EXPORT + 1u) != 0, "misaligned address must fail");
    CHECK(sr_import_lookup_export(NID_HLE, &out) != 0, "missing export must fail lookup");
    CHECK(sr_import_lookup_export(NID_HLE, NULL) != 0, "missing export must fail with NULL out");
    (void)s;
}

/* HLE mode: the seam tail-calls sr_syscall with identical (s, nid) and
 * returns its value untouched; the stub PC and bindings are irrelevant. */
static void test_hle_passthrough(void) {
    CpuState s;
    int rc;
    fresh_state(&s);
    s.r[2] = 0xAAAAAAAAu;
    rc = sr_import_call(&s, NID_HLE, 0xDEAD0000u);
    CHECK(rc == (int)s_syscall_canned, "HLE must return the handler value (got %d)", rc);
    CHECK(s_syscall_calls == 1u, "HLE must call sr_syscall exactly once");
    CHECK(s_syscall_last_nid == NID_HLE, "HLE must pass the NID through");
    CHECK(s_syscall_last_s == &s, "HLE must pass the CpuState through");
    CHECK(s.r[2] == 0xAAAAAAAAu, "seam itself must not touch registers");
    CHECK(s.flow_kind == SR_FLOW_NONE, "clean HLE traffic must not set flow");
    CHECK(sr_import_call(NULL, NID_HLE, STUB_PC) != 0, "NULL state must fail closed");
    CHECK(s_syscall_calls == 1u, "NULL state must not reach sr_syscall");
}

/* COSIM mode: HLE lane runs and the request is recorded for PR 8. */
static void test_cosim_lane(void) {
    CpuState s;
    int rc;
    fresh_state(&s);
    CHECK(sr_domain_bind_nid(SR_DOMAIN_THREADMAN, NID_COSIM) == 0, "bind the cosim NID");
    CHECK(sr_domain_mode_set(SR_DOMAIN_THREADMAN, SR_MODE_COSIM) == 0, "select COSIM");
    rc = sr_import_call(&s, NID_COSIM, STUB_PC);
    CHECK(rc == (int)s_syscall_canned, "COSIM must run the HLE lane (got %d)", rc);
    CHECK(s_syscall_calls == 1u, "COSIM must reach sr_syscall once");
    CHECK(sr_domain_cosim_request_count() == 1u, "COSIM must record the request");
    CHECK(sr_domain_last_cosim_nid() == NID_COSIM, "COSIM must record the NID");
    CHECK(s.flow_kind == SR_FLOW_NONE, "COSIM HLE lane must not set flow");
    {
        SrCosimResult r;
        sr_domain_cosim_last_result(&r);
        CHECK(r.outcome == SR_COSIM_OUTCOME_NOT_COMPARED && r.reason == SR_COSIM_REASON_UNPAIRED &&
                  r.nid == NID_COSIM,
              "a COSIM NID with no guest export is NOT_COMPARED (unpaired), never a match "
              "(outcome %d reason %d)", (int)r.outcome, (int)r.reason);
        CHECK(sr_domain_cosim_not_compared_count() == 1u && sr_domain_cosim_match_count() == 0u,
              "the unpaired call counts as not compared");
    }
    /* An unmapped NID stays on the plain HLE lane, never the cosim lane. */
    rc = sr_import_call(&s, NID_HLE, STUB_PC);
    CHECK(rc == (int)s_syscall_canned, "unmapped NID must still run HLE");
    CHECK(sr_domain_cosim_request_count() == 1u, "unmapped NID must not count as cosim");
}

/* ---- COSIM dual run (PR 8, #2148) -------------------------------------------------
 *
 * Failing-before: on the base tree COSIM runs only the HLE lane, so the guest
 * export never runs (s_export_ran stays 0) and no MATCH/DIFF/NOT_COMPARED
 * result exists; every check below fails (the result API does not build). */

#define NID_PAIR     0x6A1B2C3Du
#define NID_PAIR2    0x6A1B2C3Eu
#define NID_PAIRBAD  0x6A1B2C3Fu
#define COSIM_DATA   0x08900000u
#define COSIM_SP     0x09F00000u
#define COSIM_S0     0x00005150u
#define COSIM_A0     0x00001234u
#define EXP_SAME     (TEST_EXPORT + 0x10u)
#define EXP_BYTE     (TEST_EXPORT + 0x20u)
#define EXP_V0       (TEST_EXPORT + 0x30u)
#define EXP_REG      (TEST_EXPORT + 0x40u)
#define EXP_NESTED   (TEST_EXPORT + 0x50u)
#define EXP_VRAM     (TEST_EXPORT + 0x60u)
#define EXP_CHECKED  (TEST_EXPORT + 0x70u)
#define EXP_HELPER   (TEST_EXPORT + 0x80u)
#define COSIM_VRAM_A 0x04000100u   /* the renderer holds a newer byte here */
#define COSIM_VRAM_B 0x04001000u   /* both lanes store here */

/* Same effect as the HLE stand-in, plus a dead-stack frame and a clobbered
 * caller-saved register, neither of which the comparison may count. */
static void exp_same(CpuState *s) {
    s_export_ran++;
    MEM_W8(COSIM_DATA, 0xA5u);
    MEM_W8(s->r[29] - 16u, 0x77u);
    s->r[4] = 0xDEADBEEFu;
    s->r[2] = s_syscall_canned;
}

static void exp_byte(CpuState *s) {
    s_export_ran++;
    MEM_W8(COSIM_DATA, 0xA6u);
    s->r[2] = s_syscall_canned;
}

static void exp_v0(CpuState *s) {
    s_export_ran++;
    s->r[2] = 0x5EED0001u;
}

static void exp_reg(CpuState *s) {
    s_export_ran++;
    s->r[16] = 0x00000BADu;
    s->r[2] = s_syscall_canned;
}

/* Writes, then reaches an import the way a generated stub does. */
static void exp_nested(CpuState *s) {
    s_export_ran++;
    MEM_W8(COSIM_DATA, 0xEEu);
    sr_import_call(s, NID_HLE, STUB_PC);
    if (s->flow_kind != 0u) {
        return;
    }
    s->r[2] = s_syscall_canned;
}

/* An export that reaches the import through a computed call, as a generated
 * jalr site does (tools/codegen.py emit_checked_dispatch_call). */
static int s_helper_ran;
static int s_helper_continued;
static int s_checked_result;

static void exp_helper(CpuState *s) {
    s_helper_ran++;
    sr_import_call(s, NID_HLE, STUB_PC);
    if (s->flow_kind != 0u) {
        return;
    }
    s_helper_continued++;
}

static void exp_checked(CpuState *s) {
    s_export_ran++;
    MEM_W8(COSIM_DATA, 0xEEu);
    s_checked_result = dispatch_call_checked(s, EXP_HELPER, EXP_CHECKED + 8u);
    if (s_checked_result) {
        return;
    }
    s->r[2] = s_syscall_canned;
}

/* Same VRAM store as the HLE stand-in, made while the renderer holds newer bytes. */
static void exp_vram(CpuState *s) {
    s_export_ran++;
    MEM_W8(COSIM_VRAM_B, 0x55u);
    s->r[2] = s_syscall_canned;
}

static void cosim_fixture(CpuState *s, uint32_t export_addr) {
    fresh_state(s);
    MEM_W8(COSIM_DATA, 0u);
    MEM_W8(COSIM_SP - 16u, 0u);
    CHECK(sr_domain_bind_nid(SR_DOMAIN_MEDIA, NID_PAIR) == 0, "bind the pair NID");
    CHECK(sr_domain_mode_set(SR_DOMAIN_MEDIA, SR_MODE_COSIM) == 0, "select COSIM");
    CHECK(sr_import_register_export(NID_PAIR, export_addr) == 0, "register the pair export");
    s_hle_write_addr = COSIM_DATA;
    s_hle_write_value = 0xA5u;
    s_export_ran = 0;
    s->pc = STUB_PC;
    s->r[29] = COSIM_SP;
    s->r[16] = COSIM_S0;
    s->r[4] = COSIM_A0;
}

static void test_cosim_dual_run(void) {
    CpuState s;
    SrCosimResult r;
    int rc;

    sr_register(EXP_SAME, exp_same);
    sr_register(EXP_BYTE, exp_byte);
    sr_register(EXP_V0, exp_v0);
    sr_register(EXP_REG, exp_reg);
    sr_register(EXP_NESTED, exp_nested);
    sr_register(EXP_VRAM, exp_vram);
    sr_register(EXP_CHECKED, exp_checked);
    sr_register(EXP_HELPER, exp_helper);

    /* MATCH: both lanes run from the same input, the HLE lane is kept. */
    cosim_fixture(&s, EXP_SAME);
    rc = sr_import_call(&s, NID_PAIR, STUB_PC);
    sr_domain_cosim_last_result(&r);
    CHECK(s_export_ran == 1, "COSIM must run the guest export once (ran %d)", s_export_ran);
    CHECK(s_syscall_calls == 1u && rc == (int)s_syscall_canned,
          "COSIM must run the HLE lane once and return its value");
    CHECK(s_syscall_last_a0 == COSIM_A0,
          "the HLE lane must see the input a0, not the guest lane's (got 0x%08x)",
          s_syscall_last_a0);
    CHECK(r.outcome == SR_COSIM_OUTCOME_MATCH && sr_domain_cosim_match_count() == 1u,
          "equal effects must MATCH (outcome %d, kind %d, bytes %u)", (int)r.outcome,
          (int)r.first_kind, r.diff_bytes);
    CHECK(MEM_R8(COSIM_DATA) == 0xA5u, "the HLE lane's write is what the guest keeps");
    CHECK(MEM_R8(COSIM_SP - 16u) == 0u,
          "the guest lane's dead-stack write is rolled back (0x%02x)", MEM_R8(COSIM_SP - 16u));
    CHECK(s.r[4] == COSIM_A0 && s.r[29] == COSIM_SP && s.flow_kind == SR_FLOW_NONE,
          "CpuState after COSIM is the HLE lane's, flow clear");

    /* The dead-stack window is the only exclusion: with it off, the guest
     * lane's frame byte below $sp is a difference. */
    cosim_fixture(&s, EXP_SAME);
    sr_domain_cosim_set_dead_stack(0u);
    rc = sr_import_call(&s, NID_PAIR, STUB_PC);
    sr_domain_cosim_last_result(&r);
    CHECK(r.outcome == SR_COSIM_OUTCOME_DIFF && r.first_kind == SR_COSIM_DIFF_MEMORY &&
              r.first_addr == COSIM_SP - 16u && r.guest_byte == 0x77u && r.hle_byte == 0u &&
              r.diff_bytes == 1u,
          "without the dead-stack window the frame byte is reported (addr 0x%08x)",
          r.first_addr);

    /* A memory difference names the first byte, both values and the count;
     * the guest still sees the HLE lane's byte. */
    cosim_fixture(&s, EXP_BYTE);
    rc = sr_import_call(&s, NID_PAIR, STUB_PC);
    sr_domain_cosim_last_result(&r);
    CHECK(rc == (int)s_syscall_canned, "a DIFF still returns the HLE lane's value");
    CHECK(r.outcome == SR_COSIM_OUTCOME_DIFF && r.first_kind == SR_COSIM_DIFF_MEMORY &&
              r.first_addr == COSIM_DATA && r.guest_byte == 0xA6u && r.hle_byte == 0xA5u &&
              r.diff_bytes == 1u && r.nid == NID_PAIR && r.args[0] == COSIM_A0,
          "byte DIFF must name addr/guest/hle/count (outcome %d addr 0x%08x g 0x%02x h 0x%02x "
          "n %u)", (int)r.outcome, r.first_addr, r.guest_byte, r.hle_byte, r.diff_bytes);
    CHECK(MEM_R8(COSIM_DATA) == 0xA5u && sr_domain_cosim_diff_count() == 1u,
          "a difference never changes what the guest sees");

    /* VRAM the renderer holds newer bytes for: the snapshot reads it back first,
     * so equal stores MATCH, and the rolled-back page is committed to the
     * renderer (review S2 on #2171). Without the readback, the guest lane's
     * store pulls the renderer's byte into its kept page, the rollback puts the
     * stale byte back unseen, and the comparison reports a byte neither lane
     * wrote. */
    cosim_fixture(&s, EXP_VRAM);
    MEM_W8(COSIM_VRAM_A, 0x11u);
    MEM_W8(COSIM_VRAM_B, 0x00u);
    s_vram_page_commits = 0u;
    s_vram_truth_addr = COSIM_VRAM_A;
    s_vram_truth_value = 0x22u;
    g_sr_vram_read_pending = 1;
    s_hle_write_addr = COSIM_VRAM_B;
    s_hle_write_value = 0x55u;
    rc = sr_import_call(&s, NID_PAIR, STUB_PC);
    sr_domain_cosim_last_result(&r);
    CHECK(s_export_ran == 1 && rc == (int)s_syscall_canned, "the VRAM pair runs both lanes");
    CHECK(r.outcome == SR_COSIM_OUTCOME_MATCH,
          "equal VRAM stores MATCH while the renderer holds newer bytes (outcome %d, first "
          "0x%08x g 0x%02x h 0x%02x, %u bytes)", (int)r.outcome, r.first_addr,
          (unsigned)r.guest_byte, (unsigned)r.hle_byte, r.diff_bytes);
    CHECK(MEM_R8(COSIM_VRAM_A) == 0x22u && MEM_R8(COSIM_VRAM_B) == 0x55u,
          "after COSIM the host VRAM holds the renderer's byte and the HLE lane's store "
          "(A 0x%02x B 0x%02x)", MEM_R8(COSIM_VRAM_A), MEM_R8(COSIM_VRAM_B));
    CHECK(s_vram_page_commits >= 1u && s_vram_page_commit_addr == (COSIM_VRAM_B & ~0xFFFu),
          "the rolled-back VRAM page is committed to the renderer (%u page commits)",
          s_vram_page_commits);

    /* An HLE lane that does not end normally (its guest callback failed) is
     * not compared (review N2 on #2171). */
    cosim_fixture(&s, EXP_SAME);
    s_hle_fail_flow = 1;
    rc = sr_import_call(&s, NID_PAIR, STUB_PC);
    sr_domain_cosim_last_result(&r);
    CHECK(r.outcome == SR_COSIM_OUTCOME_NOT_COMPARED && r.reason == SR_COSIM_REASON_HLE_FLOW,
          "an HLE lane with a pending flow is NOT_COMPARED (outcome %d reason %d)",
          (int)r.outcome, (int)r.reason);
    CHECK(s.flow_kind == SR_FLOW_FATAL, "the HLE lane's flow is what the caller sees");

    /* A v0 difference with equal memory. */
    cosim_fixture(&s, EXP_V0);
    s_hle_write_addr = 0u;
    rc = sr_import_call(&s, NID_PAIR, STUB_PC);
    sr_domain_cosim_last_result(&r);
    CHECK(r.outcome == SR_COSIM_OUTCOME_DIFF && r.first_kind == SR_COSIM_DIFF_V0 &&
              r.guest_v0 == 0x5EED0001u && r.hle_v0 == s_syscall_canned && r.diff_bytes == 0u,
          "v0 DIFF must carry both values (kind %d)", (int)r.first_kind);

    /* A callee-saved register the guest lane did not preserve. */
    cosim_fixture(&s, EXP_REG);
    s_hle_write_addr = 0u;
    rc = sr_import_call(&s, NID_PAIR, STUB_PC);
    sr_domain_cosim_last_result(&r);
    CHECK(r.outcome == SR_COSIM_OUTCOME_DIFF && r.first_kind == SR_COSIM_DIFF_REGISTER &&
              r.diff_reg == 16u && r.guest_reg == 0x00000BADu && r.hle_reg == COSIM_S0,
          "callee-saved DIFF must name the register (kind %d reg %u)", (int)r.first_kind,
          r.diff_reg);
    CHECK(s.r[16] == COSIM_S0, "s0 after COSIM is the HLE lane's");

    /* The guest lane reaches an import: refused (no second host effect),
     * rolled back, reported NOT_COMPARED with that import's NID. */
    cosim_fixture(&s, EXP_NESTED);
    rc = sr_import_call(&s, NID_PAIR, STUB_PC);
    sr_domain_cosim_last_result(&r);
    CHECK(s_export_ran == 1 && s_syscall_calls == 1u && s_syscall_last_nid == NID_PAIR,
          "a nested import in the guest lane must never reach HLE (%u calls, last 0x%08x)",
          s_syscall_calls, s_syscall_last_nid);
    CHECK(r.outcome == SR_COSIM_OUTCOME_NOT_COMPARED &&
              r.reason == SR_COSIM_REASON_NESTED_IMPORT && r.nested_nid == NID_HLE,
          "a nested import is NOT_COMPARED and names the import (reason %d nid 0x%08x)",
          (int)r.reason, r.nested_nid);
    CHECK(MEM_R8(COSIM_DATA) == 0xA5u && s.flow_kind == SR_FLOW_NONE &&
              rc == (int)s_syscall_canned,
          "the refused lane is rolled back and the HLE lane's result kept");

    /* The import is reached under a computed call (dispatch_call_checked, a
     * generated jalr site): the refusal unwinds through that boundary to the
     * lane's entry, the call is NOT_COMPARED and the HLE lane runs (review S1
     * on #2171). Before the fix the checked boundary ended the process. */
    cosim_fixture(&s, EXP_CHECKED);
    rc = sr_import_call(&s, NID_PAIR, STUB_PC);
    sr_domain_cosim_last_result(&r);
    CHECK(s_export_ran == 1 && s_helper_ran == 1,
          "the export and its computed callee run in the guest lane (%d, %d)", s_export_ran,
          s_helper_ran);
    CHECK(s_helper_continued == 0, "the helper does not run past the refused import");
    CHECK(s_checked_result == 1, "dispatch_call_checked tells its caller to return (got %d)",
          s_checked_result);
    CHECK(r.outcome == SR_COSIM_OUTCOME_NOT_COMPARED &&
              r.reason == SR_COSIM_REASON_NESTED_IMPORT && r.nested_nid == NID_HLE,
          "an import under a checked call is NOT_COMPARED nested-import (outcome %d reason %d)",
          (int)r.outcome, (int)r.reason);
    CHECK(s_syscall_calls == 1u && s_syscall_last_nid == NID_PAIR && rc == (int)s_syscall_canned &&
              s.flow_kind == SR_FLOW_NONE && MEM_R8(COSIM_DATA) == 0xA5u,
          "the HLE lane runs once and its result is kept (%u calls)", s_syscall_calls);
    CHECK(sr_dispatch_unwind_requested == 0, "the unwind flag is clear after the lane");

    /* Dispatch refuses the guest export: NOT_COMPARED, never fatal, HLE kept. */
    cosim_fixture(&s, EXP_SAME);
    CHECK(sr_domain_bind_nid(SR_DOMAIN_MEDIA, NID_PAIRBAD) == 0, "bind the dangling pair");
    CHECK(sr_import_register_export(NID_PAIRBAD, UNOWNED_ADDR) == 0, "register it");
    rc = sr_import_call(&s, NID_PAIRBAD, STUB_PC);
    sr_domain_cosim_last_result(&r);
    CHECK(r.outcome == SR_COSIM_OUTCOME_NOT_COMPARED &&
              r.reason == SR_COSIM_REASON_GUEST_REJECTED,
          "a rejected guest export is NOT_COMPARED (reason %d)", (int)r.reason);
    CHECK(rc == (int)s_syscall_canned && s_syscall_calls == 1u && s.flow_kind == SR_FLOW_NONE,
          "a rejected guest lane leaves the HLE lane as the result");

    /* A cosim call inside the HLE lane of another is counted, not compared,
     * and leaves the outer call's result alone. */
    cosim_fixture(&s, EXP_SAME);
    CHECK(sr_domain_bind_nid(SR_DOMAIN_MEDIA, NID_PAIR2) == 0, "bind the inner pair");
    CHECK(sr_import_register_export(NID_PAIR2, EXP_SAME) == 0, "register the inner export");
    s_hle_reenter_nid = NID_PAIR2;
    rc = sr_import_call(&s, NID_PAIR, STUB_PC);
    sr_domain_cosim_last_result(&r);
    CHECK(s_syscall_calls == 2u && sr_domain_cosim_not_compared_count() == 1u,
          "the inner cosim call runs HLE only and counts as not compared (%u calls)",
          s_syscall_calls);
    CHECK(r.nid == NID_PAIR && r.outcome == SR_COSIM_OUTCOME_MATCH,
          "the outer result survives the inner call (nid 0x%08x outcome %d)", r.nid,
          (int)r.outcome);
    sr_domain_cosim_release();
}

/* LLE mode with a registered export: the guest body runs through the
 * production linked-call boundary and the seam reports handled. */
static void test_lle_hit(void) {
    CpuState s;
    int rc;
    fresh_state(&s);
    CHECK(sr_domain_bind_nid(SR_DOMAIN_THREADMAN, NID_LLEHIT) == 0, "bind the LLE NID");
    CHECK(sr_domain_mode_set(SR_DOMAIN_THREADMAN, SR_MODE_LLE) == 0, "select LLE");
    CHECK(sr_import_register_export(NID_LLEHIT, TEST_EXPORT) == 0, "register the export");
    sr_register(TEST_EXPORT, test_export_fn);
    s_export_ran = 0;
    s.pc = STUB_PC;
    rc = sr_import_call(&s, NID_LLEHIT, STUB_PC);
    CHECK(rc == 0, "LLE hit must report handled (got %d)", rc);
    CHECK(s_export_ran == 1, "LLE hit must run the guest export exactly once");
    CHECK(s.r[2] == 0x5EED0001u, "export result must survive (v0=0x%08x)", s.r[2]);
    CHECK(s_syscall_calls == 0u, "LLE hit must never reach HLE");
    CHECK(s.flow_kind == SR_FLOW_NONE, "clean LLE traffic must not set flow");
}

/* LLE mode without an export, and unmapped NIDs under LLE selection, fail
 * closed: negative return, FATAL flow naming the NID, HLE never reached. */
static void test_lle_miss(void) {
    CpuState s;
    int rc;
    fresh_state(&s);
    CHECK(sr_domain_bind_nid(SR_DOMAIN_THREADMAN, NID_LLE) == 0, "bind the LLE NID");
    CHECK(sr_domain_mode_set(SR_DOMAIN_THREADMAN, SR_MODE_LLE) == 0, "select LLE");
    s.pc = STUB_PC;
    rc = sr_import_call(&s, NID_LLE, STUB_PC);
    CHECK(rc != 0, "LLE miss must not report success");
    CHECK(s.flow_kind == SR_FLOW_FATAL, "LLE miss must set FATAL flow (got %u)", s.flow_kind);
    CHECK(s.flow_target == NID_LLE, "FATAL target must name the NID");
    CHECK(s_syscall_calls == 0u, "LLE miss must never reach HLE");

    /* Unmapped libraries are HLE-only even when a sibling domain runs LLE. */
    fresh_state(&s);
    CHECK(sr_domain_mode_set(SR_DOMAIN_THREADMAN, SR_MODE_LLE) == 0, "select LLE");
    rc = sr_import_call(&s, NID_HLE, STUB_PC);
    CHECK(rc == (int)s_syscall_canned, "unmapped NID must stay on HLE");
    CHECK(s.flow_kind == SR_FLOW_NONE, "HLE-only traffic must not set flow");
}

/* A registered export that dispatch rejects (no AOT body, no span) fails
 * closed instead of falling through: the interpreter lane is real here. */
static void test_lle_dispatch_reject(void) {
    CpuState s;
    int rc;
    fresh_state(&s);
    CHECK(sr_domain_bind_nid(SR_DOMAIN_THREADMAN, NID_BAD) == 0, "bind the bad NID");
    CHECK(sr_domain_mode_set(SR_DOMAIN_THREADMAN, SR_MODE_LLE) == 0, "select LLE");
    CHECK(sr_import_register_export(NID_BAD, UNOWNED_ADDR) == 0, "register the dangling export");
    s.pc = STUB_PC;
    rc = sr_import_call(&s, NID_BAD, STUB_PC);
    CHECK(rc != 0, "dispatch rejection must not report success");
    CHECK(s.flow_kind == SR_FLOW_FATAL, "dispatch rejection must set FATAL flow");
    CHECK(s.flow_target == NID_BAD, "FATAL target must name the NID");
    CHECK(s_syscall_calls == 0u, "dispatch rejection must never reach HLE");
}

/* FALLBACK mode: a miss logs once and runs HLE; a hit runs the guest with
 * no fallback accounting; a rejected dispatch fails closed (never HLE after
 * a partially executed guest body). */
static void test_fallback_lane(void) {
    CpuState s;
    int rc;
    fresh_state(&s);
    CHECK(sr_domain_bind_nid(SR_DOMAIN_IO, NID_FB) == 0, "bind the fallback NID");
    CHECK(sr_domain_bind_nid(SR_DOMAIN_IO, NID_FBHIT) == 0, "bind the fallback-hit NID");
    CHECK(sr_domain_mode_set(SR_DOMAIN_IO, SR_MODE_LLE_FALLBACK_HLE) == 0, "select fallback");
    s.pc = STUB_PC;
    rc = sr_import_call(&s, NID_FB, STUB_PC);
    CHECK(rc == (int)s_syscall_canned, "fallback miss must run HLE (got %d)", rc);
    CHECK(s_syscall_calls == 1u, "fallback miss must reach sr_syscall once");
    CHECK(sr_domain_fallback_count() == 1u, "fallback miss must be counted");
    CHECK(sr_domain_last_fallback_nid() == NID_FB, "fallback must record the NID");
    CHECK(s.flow_kind == SR_FLOW_NONE, "fallback HLE lane must not set flow");

    CHECK(sr_import_register_export(NID_FBHIT, TEST_EXPORT) == 0, "register the fallback export");
    sr_register(TEST_EXPORT, test_export_fn);
    s_export_ran = 0;
    rc = sr_import_call(&s, NID_FBHIT, STUB_PC);
    CHECK(rc == 0, "fallback hit must report handled (got %d)", rc);
    CHECK(s_export_ran == 1, "fallback hit must run the guest export");
    CHECK(s_syscall_calls == 1u, "fallback hit must never reach HLE");
    CHECK(sr_domain_fallback_count() == 1u, "fallback hit must not count as fallback");

    CHECK(sr_domain_bind_nid(SR_DOMAIN_IO, NID_BAD) == 0, "bind the dangling NID");
    CHECK(sr_import_register_export(NID_BAD, UNOWNED_ADDR) == 0, "register the dangling export");
    rc = sr_import_call(&s, NID_BAD, STUB_PC);
    CHECK(rc != 0, "fallback dispatch rejection must not report success");
    CHECK(s.flow_kind == SR_FLOW_FATAL, "fallback dispatch rejection must set FATAL flow");
    CHECK(s_syscall_calls == 1u, "fallback dispatch rejection must never reach HLE");
}

int main(void) {
    CpuState warm;
    sr_mem_init();
    sr_exec_span_reset();
    /* The production lookup only resolves bodies with executable-span
     * ownership, so the synthetic export owns one span. UNOWNED_ADDR
     * deliberately owns none: that is what makes the rejection cases
     * exercise the fail-closed miss path instead of executing bytes. */
    if (!sr_exec_span_register(TEST_EXPORT, TEST_EXPORT + 0x1000u)) {
        fprintf(stderr, "FAIL: export span rejected\n");
        return 1;
    }
    /* Warm the dispatch miss tracker off the measured paths so the
     * rejection cases below log their own miss deterministically. */
    memset(&warm, 0, sizeof warm);
    test_defaults();
    test_set_get_reset();
    test_lock();
    test_library_table();
    test_nid_bindings();
    test_export_registry();
    test_hle_passthrough();
    test_cosim_lane();
    test_cosim_dual_run();
    test_lle_hit();
    test_lle_miss();
    test_lle_dispatch_reject();
    test_fallback_lane();
    sr_domain_reset_defaults();
    if (g_failed) {
        fprintf(stderr, "domain-mode selftest: FAILED\n");
        return 1;
    }
    printf("domain-mode selftest: OK\n");
    return 0;
}
