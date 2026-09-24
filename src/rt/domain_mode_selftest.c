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
// block below mirrors src/rt/cpu_lle_selftest.c: symbols recomp.c references
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

uint32_t sr_syscall(CpuState *s, uint32_t nid) {
    s_syscall_calls++;
    s_syscall_last_nid = nid;
    s_syscall_last_s = s;
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
    /* An unmapped NID stays on the plain HLE lane, never the cosim lane. */
    rc = sr_import_call(&s, NID_HLE, STUB_PC);
    CHECK(rc == (int)s_syscall_canned, "unmapped NID must still run HLE");
    CHECK(sr_domain_cosim_request_count() == 1u, "unmapped NID must not count as cosim");
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
