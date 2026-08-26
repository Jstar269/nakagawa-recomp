// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * AOT <-> interpreter cosimulation gate.
 *
 * Both execution lanes run THE SAME source-owned guest bytes, produced by the
 * ordinary production pipeline (fixtures/cosim/generate.py -> prxload -> codegen):
 *
 *   lane AOT     the generated f_<addr> body, entered through production dispatch()
 *   lane INTERP  the production interpreter floor (src/rt/guest_interp.c) over the
 *                loaded image, entered through the same dispatch() with the cell's
 *                native body simply absent from the dispatch table
 *
 * Nothing about lane INTERP is a test mode: it is precisely the seam the AOT-gap
 * smoke exercises at build time (--omit-aot), selected here at run time so one
 * build can compare both lanes cell by cell.
 *
 * EVIDENCE TIER 2 (production helper / white-box). The production dispatch core,
 * the production interpreter and real codegen output all execute; the CpuState
 * seeding, the dispatch-table reset and the cell entry are test-specific.
 *
 * WHAT IS AND IS NOT PROVEN
 *   * Integer, memory and control-flow semantics are independently implemented in
 *     the two lanes, so a disagreement is real evidence.
 *   * Scalar FPU arithmetic is NOT independently implemented: both lanes call the
 *     same sr_fpu_* helpers from src/rt/fp_convert.h. The FPU cell therefore
 *     compares operand selection, register indexing and FCR31 threading, not the
 *     arithmetic kernel -- src/rt/fp_convert_selftest.c owns that.
 */

#include "recomp.c"   /* white-box: the real dispatch core and its dispatch table */

#include "cosim_recomp_funcs.h"  /* generated: f_<addr> declarations */
#include "cosim_cells.h"         /* generated: cell addresses and the guest layout */

#include <stdlib.h>
#include <string.h>

void sr_register_all(void);   /* generated: executable spans + every native body */

/* ---- stubs for runtime symbols recomp.c references ------------------------------- */

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

/* ---- ordered guest-write log --------------------------------------------------- */
/*
 * sr_note_mem_write() is the production last-writer hook: every guest store in
 * BOTH lanes reaches it through the same MEM_W*_PC accessors, so this list is an
 * ordered record of the lane's memory effects rather than a lane-specific probe.
 * The pre-store value is read here, before the accessor commits, giving each
 * record a real before/after pair.
 */

#define COSIM_MAX_WRITES 64

typedef struct {
    uint32_t pc;
    uint32_t address;
    uint32_t width;
    uint32_t before;
    uint32_t after;
} CosimWrite;

typedef struct {
    CosimWrite records[COSIM_MAX_WRITES];
    unsigned count;
    unsigned overflow;
} CosimWriteLog;

static CosimWriteLog *g_active_write_log = NULL;

void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t val, uint32_t pc) {
    if (!g_active_write_log) {
        return;
    }
    if (g_active_write_log->count >= COSIM_MAX_WRITES) {
        g_active_write_log->overflow++;
        return;
    }
    uint32_t before = 0u;
    if (sr_guest_span_readable(addr, width)) {
        before = width == 1u ? MEM_R8(addr) : width == 2u ? MEM_R16(addr) : MEM_R32(addr);
    }
    CosimWrite *record = &g_active_write_log->records[g_active_write_log->count++];
    record->pc = pc;
    record->address = addr;
    record->width = width;
    record->before = before;
    record->after = val;
}

/* ---- canonical instruction trace ------------------------------------------------ */
/*
 * sr_trace_open()/sr_begin()/sr_end() is the production per-instruction trace the
 * generated code already emits (TRACE_FORMAT.md). src/rt/guest_interp.c emits the
 * same records in the same order, so the two lanes produce directly comparable
 * traces and the FIRST differing line names the exact guest PC and opcode.
 */

#define COSIM_MAX_TRACE_LINES 512
#define COSIM_TRACE_LINE 512

typedef struct {
    char lines[COSIM_MAX_TRACE_LINES][COSIM_TRACE_LINE];
    unsigned count;
    unsigned truncated;
} CosimTrace;

static void trace_load(CosimTrace *trace, const char *path) {
    trace->count = 0;
    trace->truncated = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return;
    }
    char line[COSIM_TRACE_LINE * 2];
    while (fgets(line, (int)sizeof line, fp)) {
        if (line[0] == '#') {
            continue;   /* header: identical by construction in both lanes */
        }
        size_t length = strlen(line);
        while (length > 0u && (line[length - 1u] == '\n' || line[length - 1u] == '\r')) {
            line[--length] = '\0';
        }
        if (length == 0u) {
            continue;
        }
        if (trace->count >= COSIM_MAX_TRACE_LINES) {
            trace->truncated = 1;
            break;
        }
        if (length >= COSIM_TRACE_LINE) {
            trace->truncated = 1;
            length = COSIM_TRACE_LINE - 1u;
        }
        memcpy(trace->lines[trace->count], line, length);
        trace->lines[trace->count][length] = '\0';
        trace->count++;
    }
    fclose(fp);
}

/* ---- architectural state vector ------------------------------------------------- */
/*
 * One flat, ordered list of every architecturally visible 32-bit field, so a
 * comparison reports the FIRST differing field by name instead of "states differ".
 *
 * CpuState.pc is deliberately absent: it is not a shared architectural field
 * between the lanes (see the precise-PC contract below) and is asserted
 * separately, per lane, against its documented value.
 */

#define COSIM_FIELD_COUNT (32 + 2 + 2 + 32 + 128 + 16 + 3)

typedef struct {
    char name[12];
    uint32_t value;
} CosimField;

typedef struct {
    CosimField fields[COSIM_FIELD_COUNT];
    unsigned count;
} CosimVector;

static void vector_push(CosimVector *vector, const char *name, uint32_t value) {
    CosimField *field = &vector->fields[vector->count++];
    snprintf(field->name, sizeof field->name, "%s", name);
    field->value = value;
}

static void vector_pushf(CosimVector *vector, const char *format, unsigned index, uint32_t value) {
    CosimField *field = &vector->fields[vector->count++];
    snprintf(field->name, sizeof field->name, format, index);
    field->value = value;
}

static void vector_build(CosimVector *vector, const CpuState *s) {
    vector->count = 0;
    for (unsigned i = 0; i < 32u; i++) vector_pushf(vector, "r%u", i, s->r[i]);
    vector_push(vector, "hi", s->hi);
    vector_push(vector, "lo", s->lo);
    vector_push(vector, "fcr31", s->fcr31);
    vector_push(vector, "fpcond", s->fpcond);
    for (unsigned i = 0; i < 32u; i++) vector_pushf(vector, "f%u", i, s->fi[i]);
    for (unsigned i = 0; i < 128u; i++) vector_pushf(vector, "v%u", i, s->vi[i]);
    for (unsigned i = 0; i < 16u; i++) vector_pushf(vector, "vc%u", i, s->vfpuCtrl[i]);
    vector_push(vector, "status", s->status);
    vector_push(vector, "next_pc", s->next_pc);
    vector_push(vector, "delayslot", s->in_delay_slot);
}

/* ---- lane execution ------------------------------------------------------------- */

typedef enum {
    COSIM_TERM_AOT_HOST_RETURN = 0,   /* generated body returned to its host caller */
    COSIM_TERM_INTERP_HANDOFF  = 1,   /* interpreter reached a registered AOT target */
    COSIM_TERM_REJECT          = 2,   /* the dispatch core refused to execute */
} CosimTermination;

typedef struct {
    const char *lane;
    CpuState state;
    CosimVector vector;
    CosimWriteLog writes;
    CosimTrace trace;
    unsigned char window[COSIM_WINDOW_HI - COSIM_WINDOW_LO];
    CosimTermination termination;
    int dispatch_result;
    uint32_t handoff_target;
    int trampoline_entered;
} CosimLane;

static int g_trampoline_hits = 0;

/* The interpreter's handoff destination. It is deliberately inert: it exists so
 * lane INTERP has a REGISTERED AOT target to stop at, and it must not perturb any
 * guest-visible state, or the lanes would differ because of the harness. */
static void cosim_return_trampoline(CpuState *s) {
    (void)s;
    g_trampoline_hits++;
}

static uint32_t g_image_base = 0u;

/* Install the dispatch table for one lane.
 *
 * sr_register_all() is the real generated registration: it resets and installs
 * the analyzer-owned executable spans and every native body. Executable spans
 * live outside the dispatch table, so dropping the table afterwards leaves lane
 * INTERP with exactly the ownership lane AOT has and none of the bodies -- the
 * run-time equivalent of codegen's --omit-aot, applied to every cell at once.
 */
static void install_lane(int with_native_bodies) {
    memset(&g_dtab, 0, sizeof g_dtab);
    s_register_count = 0;
    sr_register_all();
    if (!with_native_bodies) {
        memset(&g_dtab, 0, sizeof g_dtab);
        s_register_count = 0;
    }
    sr_register(COSIM_RETURN, cosim_return_trampoline);
}

typedef struct {
    const char *name;
    uint32_t address;
    uint32_t words;
    const char *description;
    uint32_t fcr31;
    const char *expected_divergence;  /* NULL, or the one field that must differ */
} CosimCase;

/* Deterministic, poisoned seed.
 *
 * Every general register, FPU register and VFPU lane starts at a distinct
 * non-zero value, so a lane that fails to materialize a register shows the
 * poison rather than a plausible zero. */
static void seed_state(CpuState *s, const CosimCase *test) {
    memset(s, 0, sizeof *s);
    for (unsigned i = 1; i < 32u; i++) s->r[i] = 0xa5a50000u | i;
    for (unsigned i = 0; i < 32u; i++) s->fi[i] = 0xf0f00000u | i;
    for (unsigned i = 0; i < 128u; i++) s->vi[i] = 0xc0c00000u | i;
    for (unsigned i = 0; i < 16u; i++) s->vfpuCtrl[i] = 0xd0d00000u | i;
    s->r[0] = 0u;
    s->r[4] = COSIM_SCRATCH;    /* $a0: the scratch pointer every memory cell uses */
    s->r[29] = COSIM_STACK;     /* $sp */
    s->r[31] = COSIM_RETURN;    /* $ra: the cosim synchronization point */
    s->hi = 0x11223344u;
    s->lo = 0x55667788u;
    s->fcr31 = test->fcr31;
    /* FCC0 lives in FCR31 bit 23 and fpcond caches it; seed them coherently so a
     * lane that desynchronizes them is visible rather than pre-broken. */
    s->fpcond = (test->fcr31 >> 23) & 1u;
    /* The synthetic caller PC. COSIM_ENTRY is guest text, is not zero (which the
     * dispatch corruption guard claims) and is outside the 0x1000..0x10ff PLT
     * window (which the PLT miss policy claims), so neither policy can shadow the
     * behavior under test. */
    s->pc = COSIM_ENTRY;
    s->status = 0u;
    s->next_pc = 0u;
    s->in_delay_slot = 0u;
}

static void seed_window(void) {
    for (uint32_t address = COSIM_WINDOW_LO; address < COSIM_WINDOW_HI; address += 4u) {
        MEM_W32(address, 0xdead0000u | (address - COSIM_WINDOW_LO));
    }
}

static void run_lane(CosimLane *lane, const CosimCase *test, int with_native_bodies,
                     const char *trace_path) {
    lane->lane = with_native_bodies ? "AOT" : "INTERP";
    install_lane(with_native_bodies);

    /* Seeding order matters: the window is rewritten through the ordinary store
     * accessors, so it must happen before the write log is armed. */
    seed_window();
    memset(&lane->writes, 0, sizeof lane->writes);
    g_trampoline_hits = 0;
    seed_state(&lane->state, test);
    s_cpu = &lane->state;

    if (sr_trace_open(trace_path, "cosim", test->address) != 0) {
        fprintf(stderr, "cosim: cannot open trace file %s\n", trace_path);
        exit(2);
    }
    g_active_write_log = &lane->writes;
    g_sr_last_writer_enabled = 1;

    lane->dispatch_result = dispatch_try(&lane->state, test->address);

    g_sr_last_writer_enabled = 0;
    g_active_write_log = NULL;
    sr_trace_close();
    s_cpu = NULL;

    lane->trampoline_entered = g_trampoline_hits > 0;
    if (lane->dispatch_result < 0) {
        lane->termination = COSIM_TERM_REJECT;
        lane->handoff_target = 0u;
    } else if (lane->trampoline_entered) {
        /* The interpreter owns and advances the architectural PC, and leaves the
         * handoff destination there. */
        lane->termination = COSIM_TERM_INTERP_HANDOFF;
        lane->handoff_target = lane->state.pc;
    } else {
        /* Generated code does not materialize an architectural PC; a `jr $ra`
         * exit IS the host return, so the destination is the $ra it jumped
         * through. */
        lane->termination = COSIM_TERM_AOT_HOST_RETURN;
        lane->handoff_target = lane->state.r[31];
    }

    for (uint32_t address = COSIM_WINDOW_LO; address < COSIM_WINDOW_HI; address++) {
        lane->window[address - COSIM_WINDOW_LO] = MEM_R8(address);
    }
    vector_build(&lane->vector, &lane->state);
    trace_load(&lane->trace, trace_path);
}

/* ---- reporting ------------------------------------------------------------------ */

static int g_failures = 0;

static void report_fail(const CosimCase *test, const char *what) {
    g_failures++;
    fprintf(stderr, "\nCOSIM DIVERGENCE cell=%s (%s)\n  %s\n",
            test->name, test->description, what);
}

static void dump_cell_program(const CosimCase *test) {
    fprintf(stderr, "  guest program at 0x%08x (%u words):\n", test->address, test->words);
    for (uint32_t i = 0; i < test->words; i++) {
        const uint32_t address = test->address + i * 4u;
        fprintf(stderr, "    0x%08x  0x%08x\n", address, MEM_R32(address));
    }
}

static const char *termination_name(CosimTermination termination) {
    switch (termination) {
    case COSIM_TERM_AOT_HOST_RETURN: return "aot-host-return";
    case COSIM_TERM_INTERP_HANDOFF: return "interpreter-aot-handoff";
    default: return "dispatch-reject";
    }
}

static void dump_termination(const CosimLane *lane) {
    fprintf(stderr,
            "    lane %-6s termination=%s dispatch_result=%d handoff_target=0x%08x "
            "raw_pc=0x%08x sp=0x%08x ra=0x%08x\n",
            lane->lane, termination_name(lane->termination), lane->dispatch_result,
            lane->handoff_target, lane->state.pc, lane->state.r[29], lane->state.r[31]);
}

static void dump_write_log(const CosimLane *lane) {
    fprintf(stderr, "    lane %-6s ordered writes (%u%s):\n", lane->lane,
            lane->writes.count, lane->writes.overflow ? ", TRUNCATED" : "");
    for (unsigned i = 0; i < lane->writes.count; i++) {
        const CosimWrite *record = &lane->writes.records[i];
        fprintf(stderr, "      [%u] pc=0x%08x m%u[0x%08x] 0x%08x -> 0x%08x\n",
                i, record->pc, record->width * 8u, record->address,
                record->before, record->after);
    }
}

/* The first differing ordered memory effect, reported with the exact instruction
 * PC that produced it. */
static int compare_write_logs(const CosimCase *test, const CosimLane *a, const CosimLane *b) {
    const unsigned limit = a->writes.count < b->writes.count
        ? a->writes.count : b->writes.count;
    for (unsigned i = 0; i < limit; i++) {
        const CosimWrite *x = &a->writes.records[i];
        const CosimWrite *y = &b->writes.records[i];
        if (x->pc == y->pc && x->address == y->address && x->width == y->width &&
            x->before == y->before && x->after == y->after) {
            continue;
        }
        char detail[512];
        snprintf(detail, sizeof detail,
                 "ordered memory effect #%u differs\n"
                 "    AOT     pc=0x%08x m%u[0x%08x] 0x%08x -> 0x%08x\n"
                 "    INTERP  pc=0x%08x m%u[0x%08x] 0x%08x -> 0x%08x",
                 i, x->pc, x->width * 8u, x->address, x->before, x->after,
                 y->pc, y->width * 8u, y->address, y->before, y->after);
        report_fail(test, detail);
        return 1;
    }
    if (a->writes.count != b->writes.count) {
        const CosimLane *longer = a->writes.count > b->writes.count ? a : b;
        const CosimWrite *extra = &longer->writes.records[limit];
        char detail[512];
        snprintf(detail, sizeof detail,
                 "lane %s performed %u ordered writes, lane %s performed %u; the first\n"
                 "    unmatched one is pc=0x%08x m%u[0x%08x] 0x%08x -> 0x%08x",
                 a->lane, a->writes.count, b->lane, b->writes.count,
                 extra->pc, extra->width * 8u, extra->address, extra->before, extra->after);
        report_fail(test, detail);
        return 1;
    }
    return 0;
}

/* The first differing traced instruction. Both lanes emit the canonical
 * production trace, so this localizes a divergence to one guest instruction. */
static int compare_traces(const CosimCase *test, const CosimLane *a, const CosimLane *b) {
    const unsigned limit = a->trace.count < b->trace.count ? a->trace.count : b->trace.count;
    for (unsigned i = 0; i < limit; i++) {
        if (strcmp(a->trace.lines[i], b->trace.lines[i]) == 0) {
            continue;
        }
        char detail[2048];
        snprintf(detail, sizeof detail,
                 "instruction trace diverges at step %u\n"
                 "    AOT     %s\n"
                 "    INTERP  %s\n"
                 "    (each line is `step pc=... op=... <changed registers> <memory>`;\n"
                 "     the pc/op prefix names the exact guest instruction)",
                 i, a->trace.lines[i], b->trace.lines[i]);
        report_fail(test, detail);
        return 1;
    }
    if (a->trace.count != b->trace.count) {
        const CosimLane *longer = a->trace.count > b->trace.count ? a : b;
        char detail[2048];
        snprintf(detail, sizeof detail,
                 "lane %s executed %u traced instructions, lane %s executed %u;\n"
                 "    the first unmatched instruction is: %s",
                 a->lane, a->trace.count, b->lane, b->trace.count,
                 longer->trace.lines[limit]);
        report_fail(test, detail);
        return 1;
    }
    if (a->trace.truncated || b->trace.truncated) {
        report_fail(test, "instruction trace was truncated; the comparison is incomplete");
        return 1;
    }
    if (a->trace.count == 0u) {
        report_fail(test, "no instructions were traced; the comparison would be vacuous");
        return 1;
    }
    return 0;
}

/* The first differing architectural field, honouring exactly one declared
 * expected divergence per cell -- no more and no fewer. */
static int compare_vectors(const CosimCase *test, const CosimLane *a, const CosimLane *b) {
    int expected_seen = 0;
    int failed = 0;
    for (unsigned i = 0; i < a->vector.count; i++) {
        if (a->vector.fields[i].value == b->vector.fields[i].value) {
            continue;
        }
        const char *name = a->vector.fields[i].name;
        if (test->expected_divergence && strcmp(name, test->expected_divergence) == 0) {
            expected_seen = 1;
            continue;
        }
        if (!failed) {
            char detail[512];
            snprintf(detail, sizeof detail,
                     "architectural field %s differs: AOT=0x%08x INTERP=0x%08x",
                     name, a->vector.fields[i].value, b->vector.fields[i].value);
            report_fail(test, detail);
            failed = 1;
        }
    }
    if (test->expected_divergence && !expected_seen) {
        char detail[256];
        snprintf(detail, sizeof detail,
                 "field %s was declared to diverge between the lanes and did NOT; the\n"
                 "    comparator is no longer load-bearing for this asymmetry",
                 test->expected_divergence);
        report_fail(test, detail);
        failed = 1;
    }
    return failed;
}

static int compare_windows(const CosimCase *test, const CosimLane *a, const CosimLane *b) {
    for (unsigned i = 0; i < sizeof a->window; i++) {
        if (a->window[i] == b->window[i]) {
            continue;
        }
        char detail[256];
        snprintf(detail, sizeof detail,
                 "guest memory differs at 0x%08x: AOT=0x%02x INTERP=0x%02x",
                 COSIM_WINDOW_LO + i, a->window[i], b->window[i]);
        report_fail(test, detail);
        return 1;
    }
    return 0;
}

/* ---- precise PC contract -------------------------------------------------------- */
/*
 * CpuState.pc is NOT a shared architectural field between the lanes, and the
 * cosim would be dishonest if it pretended otherwise. The contract each lane
 * actually satisfies is pinned here, so a change to either lane's PC behavior
 * fails this gate instead of silently redefining what pc means:
 *
 *   CURRENT INSTRUCTION   lane INTERP: pc, advanced per instruction.
 *                         lane AOT:    not in CpuState. The generated code carries
 *                                      each instruction's address as a literal into
 *                                      sr_begin(), which is why the instruction
 *                                      trace -- not CpuState -- localizes an AOT
 *                                      divergence.
 *   NEXT INSTRUCTION      neither lane maintains next_pc; both leave it at its
 *                         seeded value (parity field for src/ref).
 *   BRANCH OWNER          the branch instruction owns its delay slot in both lanes:
 *                         condition and transfer target are read at the branch,
 *                         the link register is written before the slot, and the AOT
 *                         tier is not reconsulted at pc+4.
 *   DELAY SLOT            in_delay_slot is likewise unmaintained by both lanes.
 *   AOT HANDOFF           on `jr $ra`, lane AOT returns to its host caller without
 *                         writing pc; the architectural destination is $ra.
 *   INTERPRETER HANDOFF   lane INTERP writes pc = destination before dispatching
 *                         into the registered native body.
 *   EXCEPTION FUTURE      next_pc and in_delay_slot are unclaimed by both lanes and
 *                         are asserted to stay at their seeded values, so a future
 *                         COP0 BD/EPC model can define them without colliding with
 *                         an existing consumer.
 */
static int check_pc_contract(const CosimCase *test, const CosimLane *aot,
                             const CosimLane *interp) {
    int failed = 0;
    char detail[384];

    if (aot->termination != COSIM_TERM_AOT_HOST_RETURN) {
        snprintf(detail, sizeof detail,
                 "lane AOT terminated as %s; the cell must leave native code through "
                 "its host return", termination_name(aot->termination));
        report_fail(test, detail);
        failed = 1;
    }
    if (interp->termination != COSIM_TERM_INTERP_HANDOFF) {
        snprintf(detail, sizeof detail,
                 "lane INTERP terminated as %s; the cell must reach the registered "
                 "handoff target", termination_name(interp->termination));
        report_fail(test, detail);
        failed = 1;
    }
    if (aot->state.pc != COSIM_ENTRY) {
        snprintf(detail, sizeof detail,
                 "lane AOT wrote CpuState.pc (0x%08x, seeded 0x%08x). Generated code is "
                 "documented not to maintain an architectural PC; if that changed, the "
                 "precise-PC contract and every scheduler/debug consumer of pc must be "
                 "revisited", aot->state.pc, (uint32_t)COSIM_ENTRY);
        report_fail(test, detail);
        failed = 1;
    }
    if (interp->state.pc != COSIM_RETURN) {
        snprintf(detail, sizeof detail,
                 "lane INTERP left CpuState.pc at 0x%08x, expected the handoff target "
                 "0x%08x", interp->state.pc, (uint32_t)COSIM_RETURN);
        report_fail(test, detail);
        failed = 1;
    }
    if (aot->handoff_target != interp->handoff_target) {
        snprintf(detail, sizeof detail,
                 "normalized handoff target differs: AOT=0x%08x INTERP=0x%08x",
                 aot->handoff_target, interp->handoff_target);
        report_fail(test, detail);
        failed = 1;
    }
    if (aot->handoff_target != COSIM_RETURN) {
        snprintf(detail, sizeof detail,
                 "cell did not return to the cosim synchronization point "
                 "(target=0x%08x, expected 0x%08x)",
                 aot->handoff_target, (uint32_t)COSIM_RETURN);
        report_fail(test, detail);
        failed = 1;
    }
    if (aot->state.next_pc != 0u || interp->state.next_pc != 0u ||
        aot->state.in_delay_slot != 0u || interp->state.in_delay_slot != 0u) {
        report_fail(test,
                    "next_pc/in_delay_slot are no longer unclaimed by both lanes; the "
                    "future COP0 BD/EPC model now has an existing consumer to reconcile");
        failed = 1;
    }
    return failed;
}

/* ---- cases ---------------------------------------------------------------------- */

#define COSIM_CASE_ROW(name_, address_, words_, description_) \
    { #name_, address_, words_, description_, 0u, NULL },

static CosimCase g_cases[] = {
    COSIM_CELL_LIST(COSIM_CASE_ROW)
};

static const unsigned g_case_count = sizeof g_cases / sizeof g_cases[0];

/* Per-cell qualification applied to the generated table above. Everything a cell
 * needs beyond its address lives here, keyed by name, so the generated manifest
 * stays the single source of guest addresses. */
static void qualify_cases(void) {
    for (unsigned i = 0; i < g_case_count; i++) {
        CosimCase *test = &g_cases[i];
        if (strcmp(test->name, "spleak") == 0) {
            /* The one architectural asymmetry the lanes genuinely have: generated
             * code closes a callable entry with `s->r[29] = _sp_entry` on an o32
             * callee-saved-SP assumption; the interpreter executes only the
             * instructions present. Declared, so the comparator must keep
             * detecting it and must report nothing else for this cell. */
            test->expected_divergence = "r29";
        }
    }
}

/* The FPU cell is additionally run once per FCR31 rounding mode, because the
 * whole point of the #120 helper path is that the guest's mode selects the
 * result: a cosim that only ever ran the default mode would not exercise the
 * FCR31 threading it claims to compare. */
static const uint32_t g_fcr31_modes[] = {
    0x00000000u,  /* RN, FS off */
    0x00000001u,  /* RZ */
    0x00000002u,  /* RP */
    0x00000003u,  /* RM */
    0x01000000u,  /* RN with FS (flush) set */
};

/* ---- driver --------------------------------------------------------------------- */

static void load_guest_image(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "cosim: cannot open guest image %s\n", path);
        exit(2);
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "cosim: cannot size guest image %s\n", path);
        exit(2);
    }
    long size = ftell(fp);
    if (size <= 0) {
        fprintf(stderr, "cosim: guest image %s is empty\n", path);
        exit(2);
    }
    rewind(fp);
    unsigned char *bytes = malloc((size_t)size);
    if (!bytes || fread(bytes, 1, (size_t)size, fp) != (size_t)size) {
        fprintf(stderr, "cosim: cannot read guest image %s\n", path);
        exit(2);
    }
    fclose(fp);
    sr_mem_init();
    sr_load_segment(g_image_base, bytes, (uint32_t)size);
    free(bytes);
}

static int run_case(const CosimCase *test, const char *trace_dir) {
    char path_a[512];
    char path_b[512];
    snprintf(path_a, sizeof path_a, "%s/%s_fcr%08x_aot.trace",
             trace_dir, test->name, test->fcr31);
    snprintf(path_b, sizeof path_b, "%s/%s_fcr%08x_interp.trace",
             trace_dir, test->name, test->fcr31);

    static CosimLane aot;
    static CosimLane interp;
    run_lane(&aot, test, 1, path_a);
    run_lane(&interp, test, 0, path_b);

    const int before = g_failures;
    /* Ordered from most localizing to least: a traced instruction names the exact
     * guest PC, a write names the storing PC, a field names what ended up wrong. */
    int failed = 0;
    failed |= check_pc_contract(test, &aot, &interp);
    failed |= compare_traces(test, &aot, &interp);
    failed |= compare_write_logs(test, &aot, &interp);
    failed |= compare_windows(test, &aot, &interp);
    failed |= compare_vectors(test, &aot, &interp);

    if (g_failures != before) {
        dump_termination(&aot);
        dump_termination(&interp);
        dump_write_log(&aot);
        dump_write_log(&interp);
        dump_cell_program(test);
        fprintf(stderr, "  traces: %s\n          %s\n", path_a, path_b);
    }
    return failed;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,
                "usage: cosim_selftest <guest_image.bin> <image_base_hex> <trace_dir>\n");
        return 2;
    }
    g_image_base = (uint32_t)strtoul(argv[2], NULL, 16);
    if (g_image_base != COSIM_BASE) {
        fprintf(stderr,
                "cosim: image base 0x%08x does not match the generated manifest "
                "0x%08x\n", g_image_base, (uint32_t)COSIM_BASE);
        return 2;
    }
    load_guest_image(argv[1]);
    qualify_cases();

    unsigned executed = 0;
    for (unsigned i = 0; i < g_case_count; i++) {
        run_case(&g_cases[i], argv[3]);
        executed++;
    }
    for (unsigned m = 0; m < sizeof g_fcr31_modes / sizeof g_fcr31_modes[0]; m++) {
        for (unsigned i = 0; i < g_case_count; i++) {
            if (strcmp(g_cases[i].name, "fpu") != 0) {
                continue;
            }
            CosimCase mode_case = g_cases[i];
            mode_case.fcr31 = g_fcr31_modes[m];
            run_case(&mode_case, argv[3]);
            executed++;
        }
    }

    if (executed == 0u) {
        fprintf(stderr, "cosim: no comparison cases ran\n");
        return 1;
    }
    fprintf(stderr, "\ncosim: %u comparison cases, %d divergence report(s)\n",
            executed, g_failures);
    if (g_failures != 0) {
        fprintf(stderr, "cosim: FAIL\n");
        return 1;
    }
    fprintf(stderr, "cosim: OK\n");
    return 0;
}
