// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * Executable isolation tests for the two typed dispatch bindings that title
 * configuration owns: DISPATCH ALIASES and CALLBACK TERMINATORS.
 *
 * Standalone host executable, no game inputs required. The harness #includes recomp.c
 * and drives the real production dispatch core. The executable-span case enters the
 * public dispatch() wrapper itself; expected rejection cases use dispatch_try() so the
 * harness can assert the state that the wrapper would otherwise terminate on. Setup and
 * entry are test-specific (a synthetic CpuState, synthetic registered bodies), so this
 * is production-helper/white-box evidence, tier 2.
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
#ifdef _WIN32
#include <process.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

/* Source-owned executable bytes for the production interpreter-floor regression.
 * This range is disjoint from every public title fixture and from PROBE_PC/PROBE_RA.
 * The program writes a value through guest memory, reads it back, returns through
 * `jr $ra`, and increments v0 in the return delay slot:
 *
 *   addiu t0, zero, 0x1234
 *   sw    t0, 0(a0)
 *   lw    v0, 0(a0)
 *   jr    ra
 *   addiu v0, v0, 1
 *
 * Before the interpreter floor exists, dispatch() fabricates v0=0 and advances the
 * caller PC by eight, so this test is an executable FAILING_BEFORE. The future span
 * registry defines SR_HAS_EXEC_SPAN_REGISTRY and must require this explicit range;
 * interpreting arbitrary arena bytes is not an acceptable way to make the test pass. */
#define INTERP_EXEC_START 0x00600000u
#define INTERP_EXEC_END   (INTERP_EXEC_START + 20u)
#define INTERP_DATA_ADDR  0x00601000u
#define INTERP_REJECT_ADDR 0x00602000u
#define INTERP_UNOWNED_AOT_ADDR 0x00603000u
#define INTERP_PARTIAL_AOT_ADDR 0x00604000u

/* A production CALL regression: the native caller owns a live frame while the
 * interpreted callee returns to an interior continuation. The callee also owns a
 * small frame and changes $ra in its return delay slot, so matching the live $ra
 * after the delay slot is intentionally not sufficient. */
#define INTERP_CALL_EXEC_START 0x00610000u
#define INTERP_CALL_RESUME     (INTERP_CALL_EXEC_START + 0x24u)
#define INTERP_CALL_EXEC_END   (INTERP_CALL_EXEC_START + 0x3cu)
#define INTERP_CALL_DATA_ADDR  0x00611000u
#define INTERP_CALL_STACK      0x00410000u
#define INTERP_CALLER_AOT_ADDR 0x00620000u

/* Disjoint interpreter-owned tail-transfer cells. */
#define INTERP_TAIL_EXEC_START 0x00640000u
#define INTERP_TAIL_EXEC_END   (INTERP_TAIL_EXEC_START + 0x3cu)
#define INTERP_TAIL_J_TARGET   (INTERP_TAIL_EXEC_START + 0x10u)
#define INTERP_TAIL_JR_ENTRY   (INTERP_TAIL_EXEC_START + 0x20u)
#define INTERP_TAIL_JR_TARGET  (INTERP_TAIL_EXEC_START + 0x30u)

/* Source-owned high virtual executable module: the architectural class of a
 * build-time-translated extra PSP module (analyzer-owned load slots such as
 * 0x32200000) whose bytes exist only inside its own file, never in the flat
 * guest arena. Ownership of this range is lawful; interpreter fetch backing is
 * not present and must never be fabricated. Disjoint from every other fixture
 * range, probe address, and public title fixture family in this file. */
#define INTERP_HIGH_EXEC_START  0x32200000u
#define INTERP_HIGH_EXEC_END    (INTERP_HIGH_EXEC_START + 0x20u)
#define INTERP_HIGH_AOT_ADDR    (INTERP_HIGH_EXEC_START + 0x08u)
#define INTERP_HIGH_MISS_ADDR   (INTERP_HIGH_EXEC_START + 0x10u)
/* A low mapped-RAM neighbour used to prove that registering an out-of-arena
 * module span leaves ordinary low execution tiers fully working. */
#define INTERP_LOW_NEIGHBOR     0x00605000u

/* How many times a registered synthetic body was entered. */
static int g_body_hits = 0;
static void synthetic_body(CpuState *s) { (void)s; g_body_hits++; }

static void own_synthetic_aot_word(uint32_t address) {
    MEM_W32(address, 0u);
    CHECK(sr_exec_span_register(address, address + 4u),
          "synthetic AOT executable ownership failed at 0x%08x", address);
}

static int g_interp_handoff_hits = 0;
static uint32_t g_interp_handoff_v0 = 0;
static uint32_t g_interp_handoff_pc = 0;
static uint32_t g_interp_handoff_mem = 0;
static void interp_handoff_body(CpuState *s) {
    g_interp_handoff_hits++;
    g_interp_handoff_v0 = s->r[2];
    g_interp_handoff_pc = s->pc;
    g_interp_handoff_mem = MEM_R32(INTERP_DATA_ADDR);
}

typedef struct {
    int      body_ran;    /* a registered body was entered through dispatch */
    uint32_t v0;          /* s->r[2] afterwards */
    uint32_t pc;          /* s->pc afterwards */
    uint32_t pc_before;
    uint32_t ra;
    int      dispatch_result;
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
    int dispatch_result = dispatch_try(&s, target);
    Probe out;
    out.body_ran = (g_body_hits != before);
    out.v0 = s.r[2];
    out.pc = s.pc;
    out.pc_before = pc;
    out.ra = ra;
    out.dispatch_result = dispatch_result;
    return out;
}

/* dispatch() reports a completed callback walk as v0 = 1, pc = ra. Nothing else does. */
static int terminated(Probe p) { return p.v0 == 1u && p.pc == p.ra; }

/* The generic outcome for a target no binding claims. Target 0 is consumed by the
 * NULL_CALL policy hook (v0 = 0, pc = ra); anything else is rejected without fabricated
 * register/PC progress. dispatch() turns that negative result into process termination. */
static int generic_outcome(Probe p, uint32_t target) {
    if (p.body_ran) return 0;
    if (target == 0u)
        return p.dispatch_result == SR_GUEST_INTERP_AOT_HANDOFF &&
               p.v0 == 0u && p.pc == p.ra;
    return p.dispatch_result < 0 && p.v0 == 0xdeadbeefu && p.pc == p.pc_before;
}

/* ---- valid executable AOT miss: production interpreter floor ---------------------- */

static void test_valid_aot_miss_executes_guest_bytes(void) {
    static const uint32_t program[] = {
        0x24081234u, /* addiu t0, zero, 0x1234 */
        0xac880000u, /* sw    t0, 0(a0) */
        0x8c820000u, /* lw    v0, 0(a0) */
        0x03e00008u, /* jr    ra */
        0x24420001u, /* addiu v0, v0, 1 -- return delay slot */
    };
    for (size_t i = 0; i < sizeof program / sizeof program[0]; i++)
        MEM_W32(INTERP_EXEC_START + (uint32_t)(i * 4u), program[i]);
    MEM_W32(INTERP_DATA_ADDR, 0xfeedfaceu);

#ifdef SR_HAS_EXEC_SPAN_REGISTRY
    /* The implementation slice must add this explicit contract. Keeping the call
     * conditional lets the exact same source compile and fail on the pre-change tree. */
    sr_exec_span_reset();
    CHECK(sr_exec_span_register(INTERP_EXEC_START, INTERP_EXEC_END),
          "source-owned executable span registration failed");
    own_synthetic_aot_word(PROBE_RA);
#endif

    g_interp_handoff_hits = 0;
    g_interp_handoff_v0 = 0u;
    g_interp_handoff_pc = 0u;
    g_interp_handoff_mem = 0u;
    sr_register(PROBE_RA, interp_handoff_body);

    CpuState s;
    memset(&s, 0, sizeof s);
    s.pc = PROBE_PC;
    s.r[4] = INTERP_DATA_ADDR;
    s.r[29] = 0x00400000u;
    s.r[31] = PROBE_RA;
    s.r[2] = 0xdeadbeefu;

    dispatch(&s, INTERP_EXEC_START);

    CHECK(MEM_R32(INTERP_DATA_ADDR) == 0x00001234u,
          "valid executable miss did not perform the guest store (mem=0x%08x)",
          MEM_R32(INTERP_DATA_ADDR));
    CHECK(s.r[2] == 0x00001235u,
          "valid executable miss did not execute load + return delay slot (v0=0x%08x)",
          s.r[2]);
    CHECK(s.pc == PROBE_RA,
          "interpreted jr-ra did not leave the architectural resume PC at ra "
          "(pc=0x%08x ra=0x%08x)", s.pc, PROBE_RA);
    CHECK(g_interp_handoff_hits == 1,
          "interpreter did not hand control to the registered AOT destination "
          "(hits=%d)", g_interp_handoff_hits);
    CHECK(g_interp_handoff_v0 == 0x00001235u &&
          g_interp_handoff_pc == PROBE_RA &&
          g_interp_handoff_mem == 0x00001234u,
          "AOT handoff observed wrong state (v0=0x%08x pc=0x%08x mem=0x%08x)",
          g_interp_handoff_v0, g_interp_handoff_pc, g_interp_handoff_mem);
    sr_exec_span_reset();
}

static int g_call_continuation_hits = 0;
static int g_call_outer_handoff_hits = 0;
static uint32_t g_call_ra_after_callee = 0u;

static void call_outer_return_body(CpuState *s) {
    (void)s;
    g_call_outer_handoff_hits++;
}

/* This is an AOT caller-shaped production body. Its continuation is deliberately
 * native C, while the callee is fetched and executed by the production interpreter. */
static void synthetic_aot_call_body(CpuState *s) {
    const uint32_t entry_sp = s->r[29];

    s->r[29] = entry_sp - 16u;
    MEM_W32(s->r[29] + 12u, s->r[31]);
    s->r[2] = 2u;
    s->r[31] = INTERP_CALL_RESUME;
#ifdef SR_HAS_GUEST_CALL_BOUNDARY
    dispatch_call(s, INTERP_CALL_EXEC_START, INTERP_CALL_RESUME);
#else
    /* Failing-before path: the old untyped dispatch lets the interpreter run the
     * native caller continuation from the guest image before returning here. */
    dispatch(s, INTERP_CALL_EXEC_START);
#endif
    g_call_ra_after_callee = s->r[31];

    /* Interior AOT continuation: read the callee's store and perform the caller's
     * one-time tail. The same operations are present in the guest bytes at RESUME
     * so an unbounded interpreter is observably wrong, not merely differently traced. */
    s->r[3] += 0x10u;
    s->r[5] = MEM_R32(s->r[4]);
    s->r[2] += 0x100u;
    s->r[31] = MEM_R32(s->r[29] + 12u);
    s->r[29] += 16u;
    g_call_continuation_hits++;
}

static void test_aot_call_returns_before_native_continuation(void) {
    static const uint32_t program[] = {
        0x27bdfff8u, /* addiu sp, sp, -8       -- callee frame */
        0xafbf0004u, /* sw    ra, 4(sp)        -- preserve call link */
        0x240905a5u, /* addiu t1, zero, 0x5a5  -- caller-saved register */
        0xac890000u, /* sw    t1, 0(a0)        -- cross-tier store */
        0x24420007u, /* addiu v0, v0, 7        -- accumulator */
        0x8fbf0004u, /* lw    ra, 4(sp)        -- restore call link */
        0x27bd0008u, /* addiu sp, sp, 8        -- restore callee frame */
        0x03e00008u, /* jr    ra               -- return */
        0x27ff0004u, /* addiu ra, ra, 4        -- return delay mutates $ra */
        0x24630010u, /* addiu v1, v1, 0x10     -- AOT continuation */
        0x8c850000u, /* lw    a1, 0(a0)        -- AOT reads callee store */
        0x24420100u, /* addiu v0, v0, 0x100    -- AOT continuation */
        0x8fbf000cu, /* lw    ra, 12(sp)       -- restore caller link */
        0x03e00008u, /* jr    ra               -- caller return */
        0x27bd0010u, /* addiu sp, sp, 16       -- caller return delay */
    };
    const uint32_t initial_sp = INTERP_CALL_STACK;

    for (size_t i = 0; i < sizeof program / sizeof program[0]; i++)
        MEM_W32(INTERP_CALL_EXEC_START + (uint32_t)(i * 4u), program[i]);
    MEM_W32(INTERP_CALL_DATA_ADDR, 0u);

    sr_exec_span_reset();
    CHECK(sr_exec_span_register(INTERP_CALL_EXEC_START, INTERP_CALL_EXEC_END),
          "CALL regression executable span registration failed");
    own_synthetic_aot_word(PROBE_RA);
    own_synthetic_aot_word(INTERP_CALLER_AOT_ADDR);
    sr_register(PROBE_RA, call_outer_return_body);
    sr_register(INTERP_CALLER_AOT_ADDR, synthetic_aot_call_body);

    CpuState s;
    memset(&s, 0, sizeof s);
    s.pc = PROBE_PC;
    s.r[4] = INTERP_CALL_DATA_ADDR;
    s.r[29] = initial_sp;
    s.r[31] = PROBE_RA;
    s.r[2] = 0xdeadbeefu;
    s.r[3] = 0u;
    s.r[5] = 0xfeedfaceu;

    g_call_continuation_hits = 0;
    g_call_outer_handoff_hits = 0;
    g_call_ra_after_callee = 0u;
    dispatch(&s, INTERP_CALLER_AOT_ADDR);

    CHECK(g_call_continuation_hits == 1,
          "AOT continuation ran %d times instead of exactly once",
          g_call_continuation_hits);
    CHECK(g_call_outer_handoff_hits == 0,
          "CALL boundary handed the interpreted callee through the native outer return "
          "(%d handoff(s))", g_call_outer_handoff_hits);
    CHECK(MEM_R32(INTERP_CALL_DATA_ADDR) == 0x000005a5u,
          "interpreted CALL callee did not commit its store (mem=0x%08x)",
          MEM_R32(INTERP_CALL_DATA_ADDR));
    CHECK(s.r[5] == 0x000005a5u && s.r[3] == 0x00000010u,
          "AOT continuation observed wrong caller-saved/store state "
          "(a1=0x%08x v1=0x%08x)", s.r[5], s.r[3]);
    CHECK(s.r[2] == 0x00000109u,
          "CALL accumulator ran the wrong number of times (v0=0x%08x)", s.r[2]);
    CHECK(g_call_ra_after_callee == INTERP_CALL_RESUME + 4u,
          "return delay slot did not execute exactly once before boundary handoff "
          "(ra=0x%08x expected=0x%08x)",
          g_call_ra_after_callee, INTERP_CALL_RESUME + 4u);
    CHECK(s.r[29] == initial_sp && s.r[31] == PROBE_RA,
          "CALL frame/outer return state was not restored exactly "
          "(sp=0x%08x ra=0x%08x)", s.r[29], s.r[31]);
    sr_exec_span_reset();
}

static void test_interpreter_tail_transfers_remain_untyped(void) {
    sr_exec_span_reset();
    CHECK(sr_exec_span_register(INTERP_TAIL_EXEC_START, INTERP_TAIL_EXEC_END),
          "tail-transfer executable span registration failed");
    own_synthetic_aot_word(PROBE_RA);
    sr_register(PROBE_RA, call_outer_return_body);

    /* j target; delay slot; target body returns through the seeded outer $ra. */
    MEM_W32(INTERP_TAIL_EXEC_START, 0x08190004u);
    MEM_W32(INTERP_TAIL_EXEC_START + 4u, 0x24030011u);
    MEM_W32(INTERP_TAIL_J_TARGET, 0x24020022u);
    MEM_W32(INTERP_TAIL_J_TARGET + 4u, 0x03e00008u);
    MEM_W32(INTERP_TAIL_J_TARGET + 8u, 0x24000000u); /* addiu zero, zero, 0 */

    CpuState s;
    memset(&s, 0, sizeof s);
    s.pc = PROBE_PC;
    s.r[29] = INTERP_CALL_STACK;
    s.r[31] = PROBE_RA;
    s.r[2] = 0xdeadbeefu;
    g_call_outer_handoff_hits = 0;
    dispatch(&s, INTERP_TAIL_EXEC_START);
    CHECK(s.r[2] == 0x22u && s.r[3] == 0x11u &&
              g_call_outer_handoff_hits == 1,
          "tail j did not execute its delay/target/outer return exactly once "
          "(v0=0x%08x v1=0x%08x handoffs=%d)",
          s.r[2], s.r[3], g_call_outer_handoff_hits);

    /* jr t0 is the computed tail counterpart; it must not inherit CALL semantics. */
    MEM_W32(INTERP_TAIL_JR_ENTRY, 0x01000008u);
    MEM_W32(INTERP_TAIL_JR_ENTRY + 4u, 0x24030033u);
    MEM_W32(INTERP_TAIL_JR_TARGET, 0x24020044u);
    MEM_W32(INTERP_TAIL_JR_TARGET + 4u, 0x03e00008u);
    MEM_W32(INTERP_TAIL_JR_TARGET + 8u, 0x24000000u); /* addiu zero, zero, 0 */

    memset(&s, 0, sizeof s);
    s.pc = PROBE_PC;
    s.r[8] = INTERP_TAIL_JR_TARGET;
    s.r[29] = INTERP_CALL_STACK;
    s.r[31] = PROBE_RA;
    s.r[2] = 0xdeadbeefu;
    g_call_outer_handoff_hits = 0;
    dispatch(&s, INTERP_TAIL_JR_ENTRY);
    CHECK(s.r[2] == 0x44u && s.r[3] == 0x33u &&
              g_call_outer_handoff_hits == 1,
          "tail jr did not execute its delay/target/outer return exactly once "
          "(v0=0x%08x v1=0x%08x handoffs=%d)",
          s.r[2], s.r[3], g_call_outer_handoff_hits);
    sr_exec_span_reset();
}

static CpuState reject_state(void) {
    CpuState s;
    memset(&s, 0, sizeof s);
    s.pc = PROBE_PC;
    s.r[2] = 0xdeadbeefu;
    s.r[4] = INTERP_DATA_ADDR;
    s.r[29] = 0x00400000u;
    s.r[31] = PROBE_RA;
    return s;
}


/* ---------------------------------------------------------------------------
 * SEPARATE BOUNDARY, MEASURED NOT FIXED: $ra = 0 at the interpreter floor.
 *
 * The nested host->guest call marshalling in src/rt/hle.c and src/rt/mpeg.c
 * seeds $ra = 0 -- "the callee has no guest return address to jump to" -- and
 * relies on the callee being an AOT body whose C epilogue simply returns.  That
 * works.  What is NOT the same is a nested callee that is NOT translated and so
 * runs on the production interpreter floor: its architectural `jr $ra` sets
 * pc = 0, the interpreter has no CALL boundary to stop at (dispatch() passes
 * none), and pc = 0 owns no executable span.  The floor rejects, and the public
 * dispatch() wrapper turns a rejection into process termination.
 *
 * This is an ABI question, not a frame-ownership one: giving nested calls a
 * synthetic return address or a call boundary would redefine the callback ABI,
 * which the frame-isolation change deliberately does not do.  Recorded here so
 * the behaviour is measured rather than assumed, and so a later fix has a
 * failing-before specimen to point at.
 * --------------------------------------------------------------------------- */
#define NESTED_RA0_EXEC_START 0x00650000u
#define NESTED_RA0_EXEC_END   (NESTED_RA0_EXEC_START + 0x10u)

static void test_nested_call_ra_zero_cannot_return_from_the_interpreter_floor(void) {
    /* addiu v0, zero, 0x1234 ; jr ra ; nop -- the smallest translated-body shape. */
    static const uint32_t program[] = {
        0x24021234u, /* addiu v0, zero, 0x1234 */
        0x03e00008u, /* jr    ra */
        0x00000000u, /* nop (delay slot) */
        0x00000000u,
    };
    SrGuestInterpFault fault;
    CpuState s;
    SrGuestInterpResult result;
    int dispatch_result;

    for (size_t i = 0; i < sizeof program / sizeof program[0]; i++)
        MEM_W32(NESTED_RA0_EXEC_START + (uint32_t)(i * 4u), program[i]);

    sr_exec_span_reset();
    CHECK(sr_exec_span_register(NESTED_RA0_EXEC_START, NESTED_RA0_EXEC_END),
          "nested-call ra=0 span registration failed");

    /* Exactly the state ge_call_guest_rv()/call_guest3() hand a callee: zeroed,
     * three arguments, a frame $sp, $ra = 0, pc = the entry. */
    memset(&s, 0, sizeof s);
    s.r[29] = 0x09f0fd00u;
    s.r[31] = 0u;
    s.pc = NESTED_RA0_EXEC_START;
    s.vfpuCtrl[0] = 0xe4u; s.vfpuCtrl[1] = 0xe4u;

    result = sr_guest_interp_run(&s, NESTED_RA0_EXEC_START, &fault);
    CHECK(s.r[2] == 0x00001234u,
          "the interpreted nested callee did not execute its body (v0=0x%08x)", s.r[2]);
    CHECK(result != SR_GUEST_INTERP_AOT_HANDOFF && result != SR_GUEST_INTERP_CALL_RETURN,
          "MEASURED: an interpreted nested callee with $ra=0 unexpectedly returned "
          "cleanly (%s) -- if this is now a real contract, this record is stale",
          sr_guest_interp_result_name(result));
    CHECK(fault.pc == 0u,
          "MEASURED: the floor rejected somewhere other than the $ra=0 resume PC "
          "(fault_pc=0x%08x, result=%s)", fault.pc, sr_guest_interp_result_name(result));

    /* The same shape through the production dispatch core.  dispatch_try() is
     * used rather than dispatch() because the public wrapper terminates the
     * process on exactly this rejection -- which IS the finding. */
    memset(&s, 0, sizeof s);
    s.r[29] = 0x09f0fd00u;
    s.r[31] = 0u;
    s.pc = NESTED_RA0_EXEC_START;
    dispatch_result = dispatch_try(&s, NESTED_RA0_EXEC_START);
    CHECK(dispatch_result < 0,
          "MEASURED: the dispatch core accepted a nested $ra=0 interpreted return "
          "(result=%d); the public dispatch() wrapper terminates on a negative result",
          dispatch_result);

    /* Control: the identical body with a real return address resumes normally,
     * so the finding is about $ra = 0 and not about the interpreter floor. */
    g_interp_handoff_hits = 0;
    own_synthetic_aot_word(PROBE_RA);
    sr_register(PROBE_RA, interp_handoff_body);
    memset(&s, 0, sizeof s);
    s.r[29] = 0x09f0fd00u;
    s.r[31] = PROBE_RA;
    s.pc = NESTED_RA0_EXEC_START;
    result = sr_guest_interp_run(&s, NESTED_RA0_EXEC_START, &fault);
    CHECK(result == SR_GUEST_INTERP_AOT_HANDOFF && s.pc == PROBE_RA &&
              g_interp_handoff_hits == 1,
          "CONTROL: the same body with a non-zero $ra did not resume at it "
          "(%s, pc=0x%08x, hits=%d)",
          sr_guest_interp_result_name(result), s.pc, g_interp_handoff_hits);

    sr_exec_span_reset();
}

static void test_interpreter_rejects_unowned_and_invalid_fetches(void) {
    SrGuestInterpFault fault;
    CpuState s;
    CpuState before;
    SrGuestInterpResult result;

    /* Valid bytes in mapped RAM are data until an explicit executable span owns them. */
    sr_exec_span_reset();
    MEM_W32(INTERP_REJECT_ADDR, 0x24021234u); /* addiu v0, zero, 0x1234 */
    s = reject_state();
    before = s;
    result = sr_guest_interp_run(&s, INTERP_REJECT_ADDR, &fault);
    CHECK(result == SR_GUEST_INTERP_NOT_EXECUTABLE,
          "mapped unregistered RAM returned %s",
          sr_guest_interp_result_name(result));
    CHECK(memcmp(&s, &before, sizeof s) == 0,
          "mapped unregistered RAM changed CpuState");

    s = reject_state();
    before = s;
    int dispatch_result = dispatch_try(&s, INTERP_REJECT_ADDR);
    CHECK(dispatch_result == SR_GUEST_INTERP_NOT_EXECUTABLE,
          "production dispatch core did not reject mapped unregistered RAM (result=%d)",
          dispatch_result);
    CHECK(memcmp(&s, &before, sizeof s) == 0,
          "production dispatch rejection changed CpuState");

    CHECK(sr_exec_span_register(INTERP_REJECT_ADDR, INTERP_REJECT_ADDR + 8u),
          "misalignment test span registration failed");
    s = reject_state();
    before = s;
    result = sr_guest_interp_run(&s, INTERP_REJECT_ADDR + 2u, &fault);
    CHECK(result == SR_GUEST_INTERP_MISALIGNED_PC,
          "misaligned PC returned %s", sr_guest_interp_result_name(result));
    CHECK(memcmp(&s, &before, sizeof s) == 0,
          "misaligned PC changed CpuState");

    sr_exec_span_reset();
    CHECK(sr_exec_span_register(INTERP_REJECT_ADDR, INTERP_REJECT_ADDR + 2u),
          "incomplete-fetch test span registration failed");
    s = reject_state();
    before = s;
    result = sr_guest_interp_run(&s, INTERP_REJECT_ADDR, &fault);
    CHECK(result == SR_GUEST_INTERP_FETCH_BOUNDARY,
          "incomplete instruction fetch returned %s",
          sr_guest_interp_result_name(result));
    CHECK(memcmp(&s, &before, sizeof s) == 0,
          "incomplete instruction fetch changed CpuState");

    sr_exec_span_reset();
    MEM_W32(INTERP_PARTIAL_AOT_ADDR, 0u);
    CHECK(sr_exec_span_register(
              INTERP_PARTIAL_AOT_ADDR, INTERP_PARTIAL_AOT_ADDR + 2u),
          "partial AOT-fetch test span registration failed");
    sr_register(INTERP_PARTIAL_AOT_ADDR, interp_handoff_body);
    CHECK(sr_lookup(INTERP_PARTIAL_AOT_ADDR) == NULL,
          "AOT lookup accepted a span without one complete instruction");
    g_interp_handoff_hits = 0;
    s = reject_state();
    before = s;
    result = sr_guest_interp_run(&s, INTERP_PARTIAL_AOT_ADDR, &fault);
    CHECK(result == SR_GUEST_INTERP_FETCH_BOUNDARY,
          "registered partial-fetch AOT address returned %s",
          sr_guest_interp_result_name(result));
    CHECK(g_interp_handoff_hits == 0 && memcmp(&s, &before, sizeof s) == 0,
          "partial-fetch AOT rejection ran a body or changed CpuState");

    sr_exec_span_reset();
    CHECK(sr_exec_span_register(INTERP_REJECT_ADDR, INTERP_REJECT_ADDR + 4u),
          "end-boundary test span registration failed");
    s = reject_state();
    before = s;
    result = sr_guest_interp_run(&s, INTERP_REJECT_ADDR + 4u, &fault);
    CHECK(result == SR_GUEST_INTERP_FETCH_BOUNDARY,
          "end-of-span instruction fetch returned %s",
          sr_guest_interp_result_name(result));
    CHECK(memcmp(&s, &before, sizeof s) == 0,
          "end-of-span instruction fetch changed CpuState");

    MEM_W32(INTERP_REJECT_ADDR, 0xfc000000u); /* reserved primary opcode */
    s = reject_state();
    before = s;
    uint32_t data_before = MEM_R32(INTERP_DATA_ADDR);
    result = sr_guest_interp_run(&s, INTERP_REJECT_ADDR, &fault);
    CHECK(result == SR_GUEST_INTERP_UNSUPPORTED,
          "unsupported opcode returned %s", sr_guest_interp_result_name(result));
    CHECK(fault.opcode_valid && fault.pc == INTERP_REJECT_ADDR &&
          fault.opcode == 0xfc000000u,
          "unsupported opcode fault metadata is imprecise "
          "(valid=%d pc=0x%08x op=0x%08x)",
          fault.opcode_valid, fault.pc, fault.opcode);
    CHECK(memcmp(&s, &before, sizeof s) == 0 &&
          MEM_R32(INTERP_DATA_ADDR) == data_before,
          "unsupported opcode applied partial architectural side effects");

    /* AOT address registration selects a tier only after explicit executable
     * ownership. Reach a mapped, registered-but-unowned target after a normal
     * instruction and a delay slot: neither address equality nor mapped RAM may
     * let the native body run. The delay slot remains interpreter-owned. */
    sr_exec_span_reset();
    MEM_W32(INTERP_REJECT_ADDR, 0x24000001u); /* addiu zero, zero, 1 */
    MEM_W32(INTERP_REJECT_ADDR + 4u,
            0x08000000u | ((INTERP_UNOWNED_AOT_ADDR >> 2) & 0x03ffffffu));
    MEM_W32(INTERP_REJECT_ADDR + 8u, 0x24080055u); /* addiu t0, zero, 0x55 (delay) */
    CHECK(sr_exec_span_register(INTERP_REJECT_ADDR, INTERP_REJECT_ADDR + 12u),
          "registered-but-unowned AOT test span registration failed");
    sr_register(INTERP_UNOWNED_AOT_ADDR, interp_handoff_body);
    g_interp_handoff_hits = 0;
    s = reject_state();
    result = sr_guest_interp_run(&s, INTERP_REJECT_ADDR, &fault);
    CHECK(result == SR_GUEST_INTERP_NOT_EXECUTABLE,
          "registered AOT address outside executable ownership returned %s",
          sr_guest_interp_result_name(result));
    CHECK(g_interp_handoff_hits == 0,
          "AOT registration alone authorized execution (hits=%d)",
          g_interp_handoff_hits);
    CHECK(s.r[0] == 0u && s.r[8] == 0x55u && s.pc == INTERP_UNOWNED_AOT_ADDR,
          "interpreter delay/r0/target state is wrong before unowned handoff rejection "
          "(r0=0x%08x t0=0x%08x pc=0x%08x)",
          s.r[0], s.r[8], s.pc);

    s = reject_state();
    before = s;
    result = (SrGuestInterpResult)dispatch_try(&s, INTERP_UNOWNED_AOT_ADDR);
    CHECK(result == SR_GUEST_INTERP_NOT_EXECUTABLE,
          "direct dispatch treated unowned AOT registration as executable (result=%s)",
          sr_guest_interp_result_name(result));
    CHECK(g_interp_handoff_hits == 0 && memcmp(&s, &before, sizeof s) == 0,
          "direct unowned AOT rejection ran a body or changed CpuState");
    sr_exec_span_reset();
}

/* ---- high virtual module: authority without interpreter backing --------------------- */

static void test_high_virtual_module_authority_is_fail_closed(void) {
    SrGuestInterpFault fault;
    CpuState s;
    CpuState before;

    sr_exec_span_reset();

    /* Precondition: this architectural class of span lies outside the flat
     * interpreter arena. A mutant that aliases high addresses onto SR_PHYS
     * arena offsets, or enlarges the arena bound to cover them, fails here. */
    CHECK(!sr_guest_span_readable(INTERP_HIGH_EXEC_START, 4u),
          "precondition broken: 0x%08x is interpreter-readable in this build",
          INTERP_HIGH_EXEC_START);

    /* Structural ownership only: an analyzer-owned span whose bytes are never
     * copied into guest RAM must register, or every build carrying such a
     * module dies in sr_register_all() before guest dispatch. */
    CHECK(sr_exec_span_register(INTERP_HIGH_EXEC_START, INTERP_HIGH_EXEC_END),
          "analyzer-owned out-of-arena executable span was rejected");
    CHECK(sr_exec_span_register(INTERP_HIGH_EXEC_START, INTERP_HIGH_EXEC_END),
          "exact duplicate executable-span registration must stay idempotent");
    CHECK(!sr_exec_span_register(INTERP_HIGH_EXEC_START + 2u, INTERP_HIGH_EXEC_END),
          "misaligned out-of-arena span registration was accepted");

    /* Authority tier: a registered native body inside the owned high span is a
     * lawful dispatch destination. The build-time translation embodies those
     * instructions, so entering it requires ownership, not arena backing. */
    sr_register(INTERP_HIGH_AOT_ADDR, synthetic_body);
    CHECK(sr_lookup(INTERP_HIGH_AOT_ADDR) != NULL,
          "AOT lookup rejected a fully owned registered high-module entry");
    int high_hits_before = g_body_hits;
    Probe p = probe(INTERP_HIGH_AOT_ADDR, PROBE_PC, PROBE_RA);
    CHECK(p.body_ran && p.dispatch_result == SR_GUEST_INTERP_AOT_HANDOFF,
          "owned high-module AOT dispatch did not run its native body "
          "(ran=%d result=%d)", p.body_ran, p.dispatch_result);
    CHECK(g_body_hits == high_hits_before + 1,
          "owned high-module AOT body ran %d times instead of exactly once",
          g_body_hits - high_hits_before);
    CHECK(p.v0 == 0xdeadbeefu && p.pc == PROBE_PC,
          "high-module AOT body disturbed caller state (v0=0x%08x pc=0x%08x)",
          p.v0, p.pc);

    /* Registering an out-of-arena module span must not poison ordinary tiers:
     * startup-equivalent registration survives and low execution keeps working. */
    own_synthetic_aot_word(INTERP_LOW_NEIGHBOR);
    sr_register(INTERP_LOW_NEIGHBOR, synthetic_body);
    Probe low = probe(INTERP_LOW_NEIGHBOR, PROBE_PC, PROBE_RA);
    CHECK(low.body_ran && low.dispatch_result == SR_GUEST_INTERP_AOT_HANDOFF,
          "low execution broke while a high module span was registered "
          "(ran=%d result=%d)", low.body_ran, low.dispatch_result);

    /* Fetch tier: an owned high address with NO registered body and no readable
     * bytes fails closed precisely instead of reading fabricated bytes. */
    s = reject_state();
    before = s;
    int miss_result = dispatch_try(&s, INTERP_HIGH_MISS_ADDR);
    CHECK(miss_result == SR_GUEST_INTERP_MEMORY_FAULT,
          "owned unbacked unregistered fetch returned %s (result=%d)",
          sr_guest_interp_result_name((SrGuestInterpResult)miss_result),
          miss_result);
    CHECK(memcmp(&s, &before, sizeof s) == 0,
          "unbacked fetch rejection changed CpuState");

    s = reject_state();
    SrGuestInterpResult interp_result =
        sr_guest_interp_run(&s, INTERP_HIGH_MISS_ADDR, &fault);
    CHECK(interp_result == SR_GUEST_INTERP_MEMORY_FAULT,
          "interpreter returned %s for an owned unbacked fetch",
          sr_guest_interp_result_name(interp_result));
    CHECK(!fault.opcode_valid && fault.pc == INTERP_HIGH_MISS_ADDR &&
              fault.address == INTERP_HIGH_MISS_ADDR,
          "unbacked-fetch fault metadata is imprecise (valid=%d pc=0x%08x addr=0x%08x)",
          fault.opcode_valid, fault.pc, fault.address);

    /* Current static first-slice union behavior only: exact duplicates are
     * idempotent (asserted above), while a distinct overlapping analyzer span is
     * recorded alongside the first and neither range loses authority. This is
     * not a future module/segment/instance overlap contract. */
    CHECK(sr_exec_span_register(INTERP_HIGH_MISS_ADDR, INTERP_HIGH_EXEC_END),
          "distinct overlapping executable-span registration was rejected");
    CHECK(sr_exec_span_owns_fetch(INTERP_HIGH_MISS_ADDR),
          "complete fetch inside overlapping spans lost ownership");
    CHECK(sr_lookup(INTERP_HIGH_AOT_ADDR) != NULL,
          "overlapping registration invalidated the first span's owner");

    sr_exec_span_reset();
}

static int run_unregistered_dispatch_child(const char *self_path) {
#ifdef _WIN32
    const char *const argv[] = {self_path, "--unregistered-dispatch-child", NULL};
    return (int)_spawnv(_P_WAIT, self_path, argv);
#else
    pid_t child = fork();
    if (child == 0) {
        execl(self_path, self_path, "--unregistered-dispatch-child", (char *)NULL);
        _exit(127);
    }
    if (child < 0) return -1;
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
#endif
}

static void test_public_dispatch_wrapper_terminates_rejection(const char *self_path) {
    int child_status = run_unregistered_dispatch_child(self_path);
    CHECK(child_status == 1,
          "public dispatch wrapper did not terminate an unregistered executable attempt "
          "with status 1 (status=%d)", child_status);
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
        own_synthetic_aot_word(retired_alias_to);
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
        own_synthetic_aot_word(to);
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

int main(int argc, char **argv) {
    sr_mem_init();
    atomic_store(&sr_timeslice, 0);

    if (argc == 2 && strcmp(argv[1], "--unregistered-dispatch-child") == 0) {
        CpuState s = reject_state();
        sr_exec_span_reset();
        MEM_W32(INTERP_REJECT_ADDR, 0x24021234u);
        dispatch(&s, INTERP_REJECT_ADDR);
        return 99; /* fail-open: dispatch() must never return from this rejection */
    }

    const SrTitleRuntimeConfig *cfg = sr_title_config();
    fprintf(stderr, "dispatch-isolation-selftest: configuration \"%s\" "
            "(%u alias(es), %u terminator(s))\n",
            cfg->source_id, cfg->dispatch_alias_count, cfg->callback_terminator_count);

    test_generic_build_configures_no_collection();
    test_configured_build_declares_both_collections();
    test_valid_aot_miss_executes_guest_bytes();
    test_aot_call_returns_before_native_continuation();
    test_interpreter_tail_transfers_remain_untyped();
    test_interpreter_rejects_unowned_and_invalid_fetches();
    test_nested_call_ra_zero_cannot_return_from_the_interpreter_floor();
    test_high_virtual_module_authority_is_fail_closed();
    test_public_dispatch_wrapper_terminates_rejection(argv[0]);
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
