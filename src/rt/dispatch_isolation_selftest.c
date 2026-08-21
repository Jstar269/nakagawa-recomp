// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * Executable isolation tests for the two typed dispatch bindings that title
 * configuration owns: DISPATCH ALIASES and CALLBACK TERMINATORS.
 *
 * Standalone host executable, no game inputs required. The harness #includes recomp.c
 * and drives the REAL dispatch() entry point, so what is asserted is the production
 * dispatch path's observable effect on CpuState -- not a model of it. Setup and entry
 * are test-specific (a synthetic CpuState, synthetic registered bodies), so this is
 * production-helper/white-box evidence, tier 2.
 *
 * The same source is built once per title configuration by the Makefile matrix:
 *
 *   generic    -- no title configuration; both collections empty
 *   fixture-a  -- assets/titles/pspdev-phase5.json
 *   fixture-b  -- assets/titles/synthetic.json  (a DISJOINT synthetic address family)
 *
 * Almost every assertion is derived from sr_title_config() at run time rather than
 * hardcoded, so one source proves "only what THIS build configures acts, and nothing
 * else does" for all three builds. The one deliberate exception is the retired-binding
 * test: the guest addresses generic dispatch hardcoded before this configuration path
 * existed. No configuration here declares them, so every build must treat them as
 * ordinary traffic -- that is the zero-collision claim, and it can only be stated by
 * naming the numbers. They appear HERE, in a test that proves they are inert, and
 * nowhere in generic runtime code; tools/test_title_runtime_config.py enforces both
 * halves of that sentence.
 *
 * dispatch() writes diagnostics to stderr on the miss and null-call paths. That output
 * is expected; the exit code is the result.
 */

#include "recomp.c"   /* white-box: the real dispatch() and its hook tables */

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
uint32_t sr_syscall(CpuState *s, uint32_t nid) { (void)s; (void)nid; return 0u; }
void sr_yield(CpuState *s) { (void)s; }
int sr_vfpu_interp(CpuState *s, uint32_t op) { (void)s; (void)op; return 0; }
uint64_t SDL_GetTicksNS(void) { return 0u; }

/* ---- harness ---------------------------------------------------------------------- */

static int g_failures = 0;

#define CHECK(cond, ...) do {                                            \
    if (!(cond)) {                                                       \
        g_failures++;                                                    \
        fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);             \
        fprintf(stderr, __VA_ARGS__);                                    \
        fprintf(stderr, "\n");                                           \
    }                                                                    \
} while (0)

/* A synthetic call site far from every hook key, PLT window and diagnostic PC in
 * dispatch(), so a probe measures the binding under test and nothing else. */
#define PROBE_PC 0x00500000u
#define PROBE_RA 0x00500040u

/* How many times a registered synthetic body was entered. */
static int g_body_hits = 0;
static void synthetic_body(CpuState *s) { (void)s; g_body_hits++; }

typedef struct {
    int      body_ran;    /* a registered body was entered through dispatch */
    uint32_t v0;          /* s->r[2] afterwards */
    uint32_t pc;          /* s->pc afterwards */
    uint32_t pc_before;
    uint32_t ra;
} Probe;

/* Run one dispatch through a freshly initialized CpuState. */
static Probe probe(uint32_t target, uint32_t pc, uint32_t ra) {
    CpuState s;
    memset(&s, 0, sizeof s);
    s.pc = pc;
    s.r[29] = 0x00400000u;   /* a plausible stack pointer; nothing reads through it here */
    s.r[31] = ra;
    s.r[2] = 0xdeadbeefu;    /* poison: every path under test must overwrite v0 */
    int before = g_body_hits;
    dispatch(&s, target);
    Probe out;
    out.body_ran = (g_body_hits != before);
    out.v0 = s.r[2];
    out.pc = s.pc;
    out.pc_before = pc;
    out.ra = ra;
    return out;
}

/* dispatch() reports a completed callback walk as v0 = 1, pc = ra. Nothing else does. */
static int terminated(Probe p) { return p.v0 == 1u && p.pc == p.ra; }

/* The generic outcome for a target no binding claims. Target 0 is consumed by the
 * NULL_CALL policy hook (v0 = 0, pc = ra); anything else falls through to the miss path
 * (v0 = 0, pc = pc + 8). Both are distinguishable from a termination by v0 alone. */
static int generic_outcome(Probe p, uint32_t target) {
    if (p.body_ran || p.v0 != 0u) return 0;
    return target == 0u ? (p.pc == p.ra) : (p.pc == p.pc_before + 8u);
}

/* ---- the retired numbers: inert in every configuration ----------------------------- */

/* Guest addresses that generic dispatch hardcoded before this path existed. None of the
 * three matrix configurations declares any of them, so every matrix build must treat
 * them as ordinary traffic.
 *
 * Each probe is skipped when the configuration under test genuinely DECLARES that
 * binding, so this source stays correct for a configuration that legitimately owns these
 * addresses -- the title they came from. Under that configuration the binding is instead
 * asserted by test_configured_aliases_redirect / the terminator test, which is the right
 * claim to make there. What must never happen is a build that applies one of them
 * WITHOUT declaring it, and that is what the checks below rule out. */
static void test_retired_bindings_are_inert(void) {
    const uint32_t retired_alias_from = 0x00030950u;
    const uint32_t retired_alias_to   = 0x00030948u;

    if (!sr_title_config_dispatch_alias(retired_alias_from, NULL)) {
        /* Register the body the retired alias used to redirect INTO. If this build still
         * carried that redirect, this body would run. */
        sr_register(retired_alias_to, synthetic_body);
        Probe p = probe(retired_alias_from, PROBE_PC, PROBE_RA);
        CHECK(!p.body_ran, "retired tail-call alias 0x%08x still redirects into 0x%08x "
              "in configuration \"%s\", which does not declare it",
              retired_alias_from, retired_alias_to, sr_title_config()->source_id);
        CHECK(generic_outcome(p, retired_alias_from),
              "retired alias source 0x%08x is not an ordinary dispatch miss "
              "(v0=0x%08x pc=0x%08x)", retired_alias_from, p.v0, p.pc);
    }

    /* The retired null-callback terminator: target 0 at ra = 0x0003e06c. */
    if (!sr_title_config_is_callback_terminator(0u, PROBE_PC, 0x0003e06cu)) {
        Probe p = probe(0u, PROBE_PC, 0x0003e06cu);
        CHECK(!terminated(p), "retired null terminator (ra=0x0003e06c) still reports "
              "completion in configuration \"%s\"", sr_title_config()->source_id);
        CHECK(generic_outcome(p, 0u), "target 0 at the retired site is not ordinary "
              "null-call policy (v0=0x%08x pc=0x%08x)", p.v0, p.pc);
    }

    /* The retired -1 terminator: target UINT32_MAX at pc = 0x00292fa0, ra = 0x00047a0c. */
    if (!sr_title_config_is_callback_terminator(UINT32_MAX, 0x00292fa0u, 0x00047a0cu)) {
        Probe p = probe(UINT32_MAX, 0x00292fa0u, 0x00047a0cu);
        CHECK(!terminated(p), "retired -1 terminator (pc=0x00292fa0 ra=0x00047a0c) still "
              "reports completion in configuration \"%s\"", sr_title_config()->source_id);
        CHECK(generic_outcome(p, UINT32_MAX), "target -1 at the retired site is not an "
              "ordinary dispatch miss (v0=0x%08x pc=0x%08x)", p.v0, p.pc);
    }
}

/* ---- aliases: exactly what this build configures, and nothing else ------------------ */

static void test_configured_aliases_redirect(void) {
    const SrTitleRuntimeConfig *cfg = sr_title_config();
    for (unsigned i = 0; i < cfg->dispatch_alias_count; i++) {
        uint32_t from = cfg->dispatch_aliases[i].from;
        uint32_t to = cfg->dispatch_aliases[i].to;

        /* Unregistered destination: an alias must not fabricate a call. */
        Probe p = probe(from, PROBE_PC, PROBE_RA);
        CHECK(!p.body_ran, "alias 0x%08x invented a call with no body at 0x%08x", from, to);
        CHECK(generic_outcome(p, from), "alias 0x%08x with an unregistered destination is "
              "not an ordinary miss (v0=0x%08x pc=0x%08x)", from, p.v0, p.pc);

        /* Registered destination: the alias source must enter that body. */
        sr_register(to, synthetic_body);
        p = probe(from, PROBE_PC, PROBE_RA);
        CHECK(p.body_ran, "alias 0x%08x did not enter the body registered at 0x%08x", from, to);

        /* The accessor agrees with what dispatch did. */
        uint32_t resolved = 0u;
        CHECK(sr_title_config_dispatch_alias(from, &resolved) && resolved == to,
              "accessor disagrees with dispatch for alias 0x%08x", from);

        /* Off-by-one in either direction is a different address, not this alias. A
         * configured source is always 4-byte aligned, so neither neighbour can be one. */
        for (int delta = -1; delta <= 1; delta += 2) {
            uint32_t near_addr = (uint32_t)((int64_t)from + delta);
            CHECK(!sr_title_config_dispatch_alias(near_addr, NULL),
                  "alias matching is not exact: 0x%08x resolved as a neighbour of 0x%08x",
                  near_addr, from);
            Probe q = probe(near_addr, PROBE_PC, PROBE_RA);
            CHECK(!q.body_ran, "0x%08x redirected as if it were the alias source 0x%08x",
                  near_addr, from);
        }
    }
}

/* Alias sources declared by the public fixtures. A build must redirect only the sources
 * ITS OWN configuration declares; the rest are another title's business. */
static const uint32_t FOREIGN_ALIAS_SOURCES[] = {
    0x08805100u,              /* assets/titles/pspdev-phase5.json */
    0x08806100u, 0x08806104u, /* assets/titles/synthetic.json */
};

static int declared_alias_source(uint32_t addr) {
    const SrTitleRuntimeConfig *cfg = sr_title_config();
    for (unsigned i = 0; i < cfg->dispatch_alias_count; i++)
        if (cfg->dispatch_aliases[i].from == addr) return 1;
    return 0;
}

static void test_foreign_aliases_do_not_redirect(void) {
    for (size_t i = 0; i < sizeof FOREIGN_ALIAS_SOURCES / sizeof FOREIGN_ALIAS_SOURCES[0]; i++) {
        uint32_t from = FOREIGN_ALIAS_SOURCES[i];
        if (declared_alias_source(from)) continue;   /* this build's own; covered above */
        CHECK(!sr_title_config_dispatch_alias(from, NULL),
              "another title's alias source 0x%08x resolved in configuration \"%s\"",
              from, sr_title_config()->source_id);
        Probe p = probe(from, PROBE_PC, PROBE_RA);
        CHECK(!p.body_ran, "another title's alias source 0x%08x redirected in "
              "configuration \"%s\"", from, sr_title_config()->source_id);
        CHECK(generic_outcome(p, from), "another title's alias source 0x%08x is not an "
              "ordinary miss (v0=0x%08x pc=0x%08x)", from, p.v0, p.pc);
    }
}

/* ---- terminators: the sentinel is generic, the call site is not -------------------- */

static void test_configured_terminators_match_their_site_only(void) {
    const SrTitleRuntimeConfig *cfg = sr_title_config();
    for (unsigned i = 0; i < cfg->callback_terminator_count; i++) {
        const SrTitleCallbackTerminator *t = &cfg->callback_terminators[i];
        /* An unconstrained field is not compared, so any value stands in for it. */
        uint32_t pc = t->has_pc ? t->pc : PROBE_PC;
        uint32_t ra = t->has_ra ? t->ra : PROBE_RA;

        Probe p = probe(t->sentinel, pc, ra);
        CHECK(terminated(p), "configured terminator (sentinel=0x%08x pc=0x%08x ra=0x%08x) "
              "did not report completion (v0=0x%08x pc=0x%08x)",
              t->sentinel, pc, ra, p.v0, p.pc);

        /* The SAME sentinel one step away from the configured site must follow generic
         * behavior. This is the property that keeps a sentinel from becoming global. */
        if (t->has_pc) {
            Probe q = probe(t->sentinel, pc + 4u, ra);
            CHECK(!terminated(q), "sentinel 0x%08x terminated at pc=0x%08x, which is not "
                  "the configured site", t->sentinel, pc + 4u);
            CHECK(generic_outcome(q, t->sentinel), "sentinel 0x%08x off-site is not generic "
                  "(v0=0x%08x pc=0x%08x)", t->sentinel, q.v0, q.pc);
        }
        if (t->has_ra) {
            Probe q = probe(t->sentinel, pc, ra + 4u);
            CHECK(!terminated(q), "sentinel 0x%08x terminated at ra=0x%08x, which is not "
                  "the configured site", t->sentinel, ra + 4u);
            CHECK(generic_outcome(q, t->sentinel), "sentinel 0x%08x off-site is not generic "
                  "(v0=0x%08x pc=0x%08x)", t->sentinel, q.v0, q.pc);
        }
    }
}

/* Both public fixtures deliberately use the SAME sentinel values at DIFFERENT sites, so
 * a build must never terminate at a site only the other fixture declares. */
static const struct { uint32_t sentinel, pc, ra; } FOREIGN_TERMINATOR_SITES[] = {
    { 0u,          PROBE_PC,     0x08805300u },  /* pspdev-phase5 */
    { UINT32_MAX,  0x08805400u,  0x08805500u },  /* pspdev-phase5 */
    { 0u,          PROBE_PC,     0x08806300u },  /* synthetic */
    { UINT32_MAX,  0x08806400u,  0x08806500u },  /* synthetic */
};

static void test_foreign_terminator_sites_are_generic(void) {
    for (size_t i = 0; i < sizeof FOREIGN_TERMINATOR_SITES / sizeof FOREIGN_TERMINATOR_SITES[0]; i++) {
        uint32_t sentinel = FOREIGN_TERMINATOR_SITES[i].sentinel;
        uint32_t pc = FOREIGN_TERMINATOR_SITES[i].pc;
        uint32_t ra = FOREIGN_TERMINATOR_SITES[i].ra;
        /* Skip a site this build genuinely declares; it is asserted above. */
        if (sr_title_config_is_callback_terminator(sentinel, pc, ra)) continue;
        Probe p = probe(sentinel, pc, ra);
        CHECK(!terminated(p), "another title's terminator site (sentinel=0x%08x pc=0x%08x "
              "ra=0x%08x) reported completion in configuration \"%s\"",
              sentinel, pc, ra, sr_title_config()->source_id);
        CHECK(generic_outcome(p, sentinel), "another title's terminator site is not generic "
              "(sentinel=0x%08x v0=0x%08x pc=0x%08x)", sentinel, p.v0, p.pc);
    }
}

/* ---- the generic build inherits nothing -------------------------------------------- */

static void test_generic_build_configures_no_collection(void) {
    const SrTitleRuntimeConfig *cfg = sr_title_config();
    if (strcmp(cfg->source_id, "none") != 0) return;   /* not the generic build */
    CHECK(cfg->dispatch_alias_count == 0u,
          "the generic configuration declares %u dispatch alias(es)", cfg->dispatch_alias_count);
    CHECK(cfg->callback_terminator_count == 0u,
          "the generic configuration declares %u callback terminator(s)",
          cfg->callback_terminator_count);
    CHECK((cfg->valid & (SR_TITLE_CFG_DISPATCH_ALIASES | SR_TITLE_CFG_CALLBACK_TERMINATORS)) == 0u,
          "the generic configuration set a collection validity bit (valid=0x%x)", cfg->valid);
    /* Every foreign site is exercised by the tests above; the claim here is the stronger
     * one that no input at all can produce a match. */
    CHECK(!sr_title_config_is_callback_terminator(0u, PROBE_PC, PROBE_RA) &&
          !sr_title_config_is_callback_terminator(UINT32_MAX, PROBE_PC, PROBE_RA),
          "an unconfigured build matched a callback terminator");
    CHECK(!sr_title_config_dispatch_alias(0u, NULL) &&
          !sr_title_config_dispatch_alias(UINT32_MAX, NULL),
          "an unconfigured build resolved a dispatch alias");
}

/* A configured build must declare at least one of each, or the matrix would be asserting
 * emptiness three times over and prove nothing about the configured path. */
static void test_configured_build_declares_both_collections(void) {
    const SrTitleRuntimeConfig *cfg = sr_title_config();
    if (strcmp(cfg->source_id, "none") == 0) return;
    CHECK(cfg->dispatch_alias_count > 0u,
          "configuration \"%s\" declares no dispatch alias; the fixture is not exercising "
          "the configured path", cfg->source_id);
    CHECK(cfg->callback_terminator_count > 0u,
          "configuration \"%s\" declares no callback terminator", cfg->source_id);
}

int main(void) {
    /* The miss path calls exit(1) when SR_DISPATCH_FATAL is set. This harness probes
     * misses deliberately, so clear it rather than inherit an ambient value. */
#ifdef _WIN32
    _putenv("SR_DISPATCH_FATAL=");
#else
    unsetenv("SR_DISPATCH_FATAL");
#endif
    sr_mem_init();
    atomic_store(&sr_timeslice, 0);

    const SrTitleRuntimeConfig *cfg = sr_title_config();
    fprintf(stderr, "dispatch-isolation-selftest: configuration \"%s\" "
            "(%u alias(es), %u terminator(s))\n",
            cfg->source_id, cfg->dispatch_alias_count, cfg->callback_terminator_count);

    test_generic_build_configures_no_collection();
    test_configured_build_declares_both_collections();
    test_retired_bindings_are_inert();
    test_configured_aliases_redirect();
    test_foreign_aliases_do_not_redirect();
    test_configured_terminators_match_their_site_only();
    test_foreign_terminator_sites_are_generic();

    if (g_failures != 0) {
        fprintf(stderr, "dispatch-isolation-selftest: %d FAILURE(S) in configuration \"%s\"\n",
                g_failures, cfg->source_id);
        return 1;
    }
    fprintf(stderr, "dispatch-isolation-selftest: OK (configuration \"%s\")\n", cfg->source_id);
    return 0;
}
