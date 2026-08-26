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

/* The C newline escape, spelled once so the sources that generate this file
 * never have to round-trip a backslash through a shell. */
#define NEWLINE "\n"

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

/* COSIM_FIELD_COUNT is hand-derived from the CpuState layout, so a field added
 * to the vector without updating it would run off the end silently. Fail loudly
 * instead: this is a comparator, and a comparator that corrupts its own storage
 * reports whatever happens to be adjacent. */
static void vector_push(CosimVector *vector, const char *name, uint32_t value) {
    if (vector->count >= COSIM_FIELD_COUNT) {
        fprintf(stderr,
                "cosim: architectural vector overflow at field '%s' (capacity %u). "
                "COSIM_FIELD_COUNT no longer matches vector_build()." NEWLINE,
                name, (unsigned)COSIM_FIELD_COUNT);
        exit(2);
    }
    CosimField *field = &vector->fields[vector->count++];
    snprintf(field->name, sizeof field->name, "%s", name);
    field->value = value;
}

static void vector_pushf(CosimVector *vector, const char *format, unsigned index, uint32_t value) {
    if (vector->count >= COSIM_FIELD_COUNT) {
        fprintf(stderr, "cosim: architectural vector overflow (capacity %u)" NEWLINE,
                (unsigned)COSIM_FIELD_COUNT);
        exit(2);
    }
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
/* Lane MIXED: every native body EXCEPT one.
 *
 * This is the tier boundary the production AOT-gap floor actually crosses --
 * native caller, dispatch miss, production interpreter, and back. The omitted
 * body is removed by rebuilding the table from a snapshot rather than by
 * punching a hole in it, because the main table is open-addressed and clearing
 * an interior slot would truncate the probe chain of any key that collided
 * through it. */
typedef struct {
    uint32_t addr;
    RecompFn fn;
} CosimRegistration;

#define COSIM_MAX_REGISTRATIONS 256

static CosimRegistration g_registrations[COSIM_MAX_REGISTRATIONS];
static unsigned g_registration_count;

static void snapshot_registrations(void) {
    g_registration_count = 0;
    for (uint32_t i = 0; i < SR_DTAB_SIZE; i++) {
        if (atomic_load_explicit(&g_dtab.main[i].state, memory_order_relaxed) != 1u) {
            continue;
        }
        if (g_registration_count >= COSIM_MAX_REGISTRATIONS) {
            fprintf(stderr, "cosim: registration snapshot overflow" NEWLINE);
            exit(2);
        }
        g_registrations[g_registration_count].addr =
            atomic_load_explicit(&g_dtab.main[i].addr, memory_order_relaxed);
        g_registrations[g_registration_count].fn =
            (RecompFn)atomic_load_explicit(&g_dtab.main[i].fn, memory_order_relaxed);
        g_registration_count++;
    }
}

static void install_lane_mixed(uint32_t omit_address) {
    memset(&g_dtab, 0, sizeof g_dtab);
    s_register_count = 0;
    sr_register_all();
    snapshot_registrations();

    int found = 0;
    for (unsigned i = 0; i < g_registration_count; i++) {
        if (g_registrations[i].addr == omit_address) {
            found = 1;
        }
    }
    if (!found) {
        /* The cell would silently become an ordinary all-native run and every
         * cross-tier assertion below would pass without crossing anything. */
        fprintf(stderr,
                "cosim: cross-tier omission 0x%08x is not a registered body" NEWLINE,
                omit_address);
        exit(2);
    }

    memset(&g_dtab, 0, sizeof g_dtab);
    s_register_count = 0;
    for (unsigned i = 0; i < g_registration_count; i++) {
        if (g_registrations[i].addr == omit_address) {
            continue;
        }
        sr_register(g_registrations[i].addr, g_registrations[i].fn);
    }
    sr_register(COSIM_RETURN, cosim_return_trampoline);
}

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

/* A declared, self-retiring architectural asymmetry.
 *
 * `fields` is the COMPLETE set of architectural fields allowed to differ, and it
 * is enforced in BOTH directions: a field that stops differing fails the gate
 * just as loudly as an extra one that starts. That is what makes a known-defect
 * declaration retire itself -- when the production repair lands, this gate goes
 * red until the declaration is deleted. */
typedef struct {
    const char *const *fields;        /* NULL-terminated */
    CosimTermination termination;     /* the lane-B termination this defect forces */
    const char *reference;            /* what has to change for this to retire */
} CosimKnownDefect;

typedef struct {
    const char *name;
    uint32_t address;
    uint32_t words;
    const char *description;
    uint32_t cross_tier_omit;         /* 0, or the body lane MIXED must drop */
    uint32_t fcr31;
    const CosimKnownDefect *known_defect;
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

typedef enum {
    COSIM_LANE_INTERP = 0,   /* no native bodies at all */
    COSIM_LANE_AOT    = 1,   /* every native body */
    COSIM_LANE_MIXED  = 2,   /* every native body except the cell's omission */
} CosimLaneMode;

static void run_lane(CosimLane *lane, const CosimCase *test, int mode,
                     const char *trace_path) {
    lane->lane = mode == COSIM_LANE_AOT ? "AOT"
               : mode == COSIM_LANE_INTERP ? "INTERP"
               : "MIXED";
    if (mode == COSIM_LANE_MIXED) {
        install_lane_mixed(test->cross_tier_omit);
    } else {
        install_lane(mode);
    }

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
    if (a->writes.overflow || b->writes.overflow) {
        /* The trace channel already fails closed on truncation; the ordered-write
         * channel must too, or a divergence past the cap would be invisible to
         * it while the log still printed a plausible-looking prefix. */
        char detail[256];
        snprintf(detail, sizeof detail,
                 "the ordered write log overflowed (%s dropped %u, %s dropped %u); "
                 "the memory-effect comparison is incomplete",
                 a->lane, a->writes.overflow, b->lane, b->writes.overflow);
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
        if (test->known_defect) {
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
        if (test->known_defect) {
            /* A declared defect changes how many instructions run; the
             * architectural vector is what pins its exact shape. Report it so it
             * stays visible, but do not double-count it as a second failure. */
            fprintf(stderr,
                    "  cosim: cell %s executed %u instructions in lane %s vs %u in "
                    "lane %s (declared defect)" NEWLINE,
                    test->name, a->trace.count, a->lane, b->trace.count, b->lane);
            return 0;
        }
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
static int declared_field(const CosimCase *test, const char *name) {
    if (!test->known_defect) {
        return -1;
    }
    for (unsigned i = 0; test->known_defect->fields[i]; i++) {
        if (strcmp(test->known_defect->fields[i], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int compare_vectors(const CosimCase *test, const CosimLane *a, const CosimLane *b) {
    int failed = 0;
    unsigned char seen[16];
    memset(seen, 0, sizeof seen);

    for (unsigned i = 0; i < a->vector.count; i++) {
        if (a->vector.fields[i].value == b->vector.fields[i].value) {
            continue;
        }
        const char *name = a->vector.fields[i].name;
        const int declared = declared_field(test, name);
        if (declared >= 0 && (unsigned)declared < sizeof seen) {
            seen[declared] = 1;
            continue;
        }
        if (!failed) {
            char detail[512];
            snprintf(detail, sizeof detail,
                     "architectural field %s differs: %s=0x%08x %s=0x%08x",
                     name, a->lane, a->vector.fields[i].value,
                     b->lane, b->vector.fields[i].value);
            report_fail(test, detail);
            failed = 1;
        }
    }

    if (test->known_defect) {
        for (unsigned i = 0; test->known_defect->fields[i]; i++) {
            if (i < sizeof seen && seen[i]) {
                continue;
            }
            char detail[512];
            snprintf(detail, sizeof detail,
                     "field %s was DECLARED to diverge and did NOT.\n"
                     "    Either the comparator stopped observing this asymmetry, or the\n"
                     "    defect it stands for was repaired -- in which case delete the\n"
                     "    declaration. Retirement criterion: %s",
                     test->known_defect->fields[i], test->known_defect->reference);
            report_fail(test, detail);
            failed = 1;
        }
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
    /* Lane B's expected termination depends on what it IS.
     *
     *   INTERP  interprets everything and stops at the registered trampoline.
     *   MIXED   re-enters native code, so a correct cross-tier run ends exactly
     *           as lane AOT does -- through the caller's own host return. A
     *           declared defect may force the interpreter-handoff shape instead,
     *           and that expectation is enforced in both directions below.
     */
    const CosimTermination expected_b =
        test->cross_tier_omit == 0u
            ? COSIM_TERM_INTERP_HANDOFF
            : (test->known_defect ? test->known_defect->termination
                                  : COSIM_TERM_AOT_HOST_RETURN);
    if (interp->termination != expected_b) {
        snprintf(detail, sizeof detail,
                 "lane %s terminated as %s, expected %s", interp->lane,
                 termination_name(interp->termination), termination_name(expected_b));
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
    /* Lane INTERP owns and advances the architectural pc and leaves the handoff
     * destination in it. Lane MIXED only reaches the interpreter for part of the
     * run, so pc is whatever the last interpreted step left; it is not a shared
     * observable and is not asserted here -- the handoff target below is. */
    if (test->cross_tier_omit == 0u && interp->state.pc != COSIM_RETURN) {
        snprintf(detail, sizeof detail,
                 "lane INTERP left CpuState.pc at 0x%08x, expected the handoff target "
                 "0x%08x", interp->state.pc, (uint32_t)COSIM_RETURN);
        report_fail(test, detail);
        failed = 1;
    }
    if (test->known_defect == NULL && aot->handoff_target != interp->handoff_target) {
        snprintf(detail, sizeof detail,
                 "normalized handoff target differs: AOT=0x%08x INTERP=0x%08x",
                 aot->handoff_target, interp->handoff_target);
        report_fail(test, detail);
        failed = 1;
    }
    /* PRECONDITION for normalizing lane AOT's handoff target to $ra.
     *
     * Generated code has no architectural PC, so the harness reads $ra at
     * TERMINATION and calls it the destination. That is only the destination the
     * transfer actually latched while nothing rewrote $ra afterwards -- a return
     * delay slot that writes $ra would make the normalization report a target
     * control never went to, and manufacture a divergence out of the harness
     * rather than the code under test.
     *
     * The canonical trace already carries the answer: the final delay slot's line
     * lists the registers it changed. If $ra is among them, this normalization is
     * not valid for the cell and the gate says so instead of quietly comparing
     * the wrong thing. */
    if (aot->trace.count >= 1u) {
        const char *last = aot->trace.lines[aot->trace.count - 1u];
        if (strstr(last, " r31=") != NULL) {
            snprintf(detail, sizeof detail,
                     "the cell's final delay slot writes $ra (%s). Lane AOT's handoff "
                     "target is normalized FROM $ra at termination, so this cell would "
                     "compare a destination control never reached; latch the transfer "
                     "target instead of rewriting $ra in a return slot", last);
            report_fail(test, detail);
            failed = 1;
        }
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

#define COSIM_CASE_ROW(name_, address_, words_, description_, omit_) \
    { #name_, address_, words_, description_, omit_, 0u, NULL },

static CosimCase g_cases[] = {
    COSIM_CELL_LIST(COSIM_CASE_ROW)
};

static const unsigned g_case_count = sizeof g_cases / sizeof g_cases[0];

/* Per-cell qualification applied to the generated table above. Everything a cell
 * needs beyond its address lives here, keyed by name, so the generated manifest
 * stays the single source of guest addresses. */
/* The o32 callee-saved-SP asymmetry: generated code closes a callable entry with
 * `s->r[29] = _sp_entry`; the interpreter executes only the instructions present. */
static const char *const SPLEAK_FIELDS[] = { "r29", NULL };
static const CosimKnownDefect SPLEAK_DEFECT = {
    SPLEAK_FIELDS,
    COSIM_TERM_INTERP_HANDOFF,
    "the generated entry epilogue stops assuming an o32 callee-saved $sp",
};

/* The cross-tier RETURNING call carried a declared, self-retiring CosimKnownDefect
 * here until `rt: preserve AOT continuations across interpreter calls` (#127).
 *
 * Generated code emits `dispatch(s, _t);` for a jal/jalr with no host return after
 * it, so the caller's native frame is still live; the interpreter used to stop only
 * at a REGISTERED entry, so the callee's `jr $ra` landed in that caller's interior
 * and the caller's tail ran twice -- double-counting its work and reloading $ra
 * from an already-popped frame. The repair gives the interpreter an explicit call
 * boundary, and lane MIXED now terminates exactly as lane AOT does.
 *
 * The declaration is deleted rather than relaxed, which is the whole point of
 * enforcing one in both directions: this gate went red the moment the fields
 * stopped diverging, and stayed red until the claim was removed. `xcall` is now an
 * ordinary PASS and any regression is an ordinary failure. */

static void qualify_cases(void) {
    for (unsigned i = 0; i < g_case_count; i++) {
        CosimCase *test = &g_cases[i];
        if (strcmp(test->name, "spleak") == 0) {
            test->known_defect = &SPLEAK_DEFECT;
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
    snprintf(path_b, sizeof path_b, "%s/%s_fcr%08x_%s.trace",
             trace_dir, test->name, test->fcr31,
             test->cross_tier_omit ? "mixed" : "interp");

    static CosimLane aot;
    static CosimLane interp;
    run_lane(&aot, test, COSIM_LANE_AOT, path_a);
    run_lane(&interp, test,
             test->cross_tier_omit ? COSIM_LANE_MIXED : COSIM_LANE_INTERP,
             path_b);

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

/* ---- fail-closed negative corpus ------------------------------------------------ */
/*
 * The comparison above only ever exercises SUCCESSFUL execution. The production
 * interpreter's central claim is the opposite one -- that it refuses rather than
 * fabricates -- and a mutation campaign against the two-lane comparator cannot
 * see that claim at all: every guard below can be deleted while every cell still
 * agrees, because no cell ever reaches one.
 *
 * These are therefore INTERPRETER-TIER assertions, not lane comparisons. Lane AOT
 * is deliberately more permissive (MEM_* absorbs an out-of-range access through
 * sr_oor() instead of stopping), and teaching it to manufacture matching faults
 * purely so the corpus could be expressed as a cosim cell would be fabricating
 * agreement. Each case asserts four things:
 *
 *   1. the exact SrGuestInterpResult classification;
 *   2. the fault record (pc, opcode where valid, address);
 *   3. no illicit guest write -- the whole observed window is byte-compared;
 *   4. no silent continuation -- architectural state changed in exactly the
 *      declared ways and no others.
 */

static uint32_t enc_r(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sh, uint32_t fn) {
    return ((rs & 31u) << 21) | ((rt & 31u) << 16) | ((rd & 31u) << 11) |
           ((sh & 31u) << 6) | (fn & 63u);
}

static uint32_t enc_i(uint32_t op, uint32_t rs, uint32_t rt, uint32_t imm) {
    return ((op & 63u) << 26) | ((rs & 31u) << 21) | ((rt & 31u) << 16) | (imm & 0xffffu);
}

static uint32_t enc_j(uint32_t op, uint32_t target) {
    return ((op & 63u) << 26) | ((target >> 2) & 0x03ffffffu);
}

typedef struct {
    const char *name;
    const char *guard;             /* the fail-closed guard this case stands for */
    uint32_t words[4];
    unsigned word_count;
    uint32_t entry_offset;         /* bytes into negpad, or COSIM_NEGPAD_OFFSET_UNOWNED */
    int expect_result;
    uint32_t expect_fault_pc;      /* relative to negpad unless unowned */
    int expect_opcode_valid;
    unsigned expect_opcode_index;  /* which patched word the fault must name */
    uint32_t expect_address;       /* absolute, or 0 to mean "the faulting pc" */
    const char *allowed_field;     /* the one field this case may legitimately change */
    uint32_t allowed_value;
} CosimNegative;

#define NEG_UNOWNED 0xffffffffu

static int g_negative_failures = 0;

static void negative_fail(const CosimNegative *test, const char *what) {
    g_negative_failures++;
    fprintf(stderr, NEWLINE "COSIM NEGATIVE FAIL case=%s (%s)" NEWLINE "  %s" NEWLINE,
            test->name, test->guard, what);
}

static int run_negative(const CosimNegative *test) {
    char detail[512];

    /* Lane INTERP's installation: executable spans registered, no native bodies,
     * so the interpreter owns these bytes exactly as it owns an AOT gap. */
    install_lane(0);
    seed_window();

    for (unsigned i = 0; i < test->word_count; i++) {
        MEM_W32(COSIM_NEGPAD + test->entry_offset + i * 4u, test->words[i]);
    }

    CpuState state;
    CosimCase seed = { test->name, 0u, 0u, test->guard, 0u, 0u, NULL };
    seed_state(&state, &seed);
    /* $a0 addresses memory that is neither readable nor writable, so the bounded
     * access cases fault on authority rather than on alignment. */
    state.r[4] = COSIM_UNOWNED;

    CosimVector before;
    CosimVector after;
    vector_build(&before, &state);

    unsigned char window_before[COSIM_WINDOW_HI - COSIM_WINDOW_LO];
    for (uint32_t a = COSIM_WINDOW_LO; a < COSIM_WINDOW_HI; a++) {
        window_before[a - COSIM_WINDOW_LO] = MEM_R8(a);
    }

    const uint32_t entry = test->entry_offset == NEG_UNOWNED
        ? (uint32_t)COSIM_UNOWNED
        : (uint32_t)COSIM_NEGPAD + test->entry_offset;

    SrGuestInterpFault fault;
    memset(&fault, 0, sizeof fault);
    s_cpu = &state;
    const SrGuestInterpResult result = sr_guest_interp_run(&state, entry, &fault);
    s_cpu = NULL;

    int failed = 0;

    if ((int)result != test->expect_result) {
        snprintf(detail, sizeof detail,
                 "result was %s (%d), expected %d. A guard that stops rejecting is "
                 "exactly the regression this case exists to catch",
                 sr_guest_interp_result_name(result), (int)result, test->expect_result);
        negative_fail(test, detail);
        failed = 1;
    }

    const uint32_t expect_pc = test->entry_offset == NEG_UNOWNED
        ? (uint32_t)COSIM_UNOWNED
        : (uint32_t)COSIM_NEGPAD + test->expect_fault_pc;
    if (fault.pc != expect_pc) {
        snprintf(detail, sizeof detail, "fault.pc=0x%08x, expected 0x%08x",
                 fault.pc, expect_pc);
        negative_fail(test, detail);
        failed = 1;
    }

    if (fault.opcode_valid != test->expect_opcode_valid) {
        snprintf(detail, sizeof detail, "fault.opcode_valid=%d, expected %d",
                 fault.opcode_valid, test->expect_opcode_valid);
        negative_fail(test, detail);
        failed = 1;
    } else if (test->expect_opcode_valid) {
        const uint32_t want = test->words[test->expect_opcode_index];
        if (fault.opcode != want) {
            snprintf(detail, sizeof detail,
                     "fault.opcode=0x%08x, expected the rejected word 0x%08x",
                     fault.opcode, want);
            negative_fail(test, detail);
            failed = 1;
        }
    }

    const uint32_t expect_address = test->expect_address ? test->expect_address : expect_pc;
    if (fault.address != expect_address) {
        snprintf(detail, sizeof detail, "fault.address=0x%08x, expected 0x%08x",
                 fault.address, expect_address);
        negative_fail(test, detail);
        failed = 1;
    }

    for (uint32_t a = COSIM_WINDOW_LO; a < COSIM_WINDOW_HI; a++) {
        const unsigned char now = MEM_R8(a);
        if (now == window_before[a - COSIM_WINDOW_LO]) {
            continue;
        }
        snprintf(detail, sizeof detail,
                 "a REJECTED sequence wrote guest memory at 0x%08x (0x%02x -> 0x%02x); "
                 "rejection must leave guest state exactly as it was",
                 a, window_before[a - COSIM_WINDOW_LO], now);
        negative_fail(test, detail);
        failed = 1;
        break;
    }

    vector_build(&after, &state);
    for (unsigned i = 0; i < after.count; i++) {
        if (before.fields[i].value == after.fields[i].value) {
            continue;
        }
        const char *name = after.fields[i].name;
        if (test->allowed_field && strcmp(name, test->allowed_field) == 0 &&
            after.fields[i].value == test->allowed_value) {
            continue;
        }
        snprintf(detail, sizeof detail,
                 "architectural field %s changed 0x%08x -> 0x%08x across a REJECTED "
                 "sequence", name, before.fields[i].value, after.fields[i].value);
        negative_fail(test, detail);
        failed = 1;
        break;
    }
    if (test->allowed_field) {
        int seen = 0;
        for (unsigned i = 0; i < after.count; i++) {
            if (strcmp(after.fields[i].name, test->allowed_field) == 0) {
                seen = after.fields[i].value == test->allowed_value;
            }
        }
        if (!seen) {
            snprintf(detail, sizeof detail,
                     "field %s was declared to hold 0x%08x after the rejection and does "
                     "not; the ordering this case pins no longer holds",
                     test->allowed_field, test->allowed_value);
            negative_fail(test, detail);
            failed = 1;
        }
    }

    if (!failed) {
        fprintf(stderr, "  cosim negative OK  %-22s %s" NEWLINE, test->name, test->guard);
    }
    return failed;
}

/* Every control encoding, in a delay slot, one at a time.
 *
 * `control-in-delay-slot` above pins one representative. This sweep pins the
 * whole class, because the property that matters is not "a `j` is rejected" but
 * "no control encoding is ever decoded as arithmetic" -- and the bit fields of a
 * branch overlap real arithmetic forms, so a single representative would leave
 * the interesting encodings untested.
 *
 * NOTE ON LAYERING: deleting the is_control_opcode() guard inside
 * execute_noncontrol() does NOT make this sweep fail, and that is correct rather
 * than a hole. Every control encoding also lacks an arithmetic handler, so both
 * layers independently reject it today. The guard becomes the load-bearing one
 * the moment a primary opcode is implemented that a control encoding shares --
 * which is exactly when this sweep starts distinguishing them. It asserts the
 * PROPERTY, not one implementation of it.
 */
static int run_control_encoding_sweep(void) {
    const struct { const char *name; uint32_t word; } control[] = {
        { "j",      enc_j(0x02u, (uint32_t)COSIM_NEGPAD) },
        { "jal",    enc_j(0x03u, (uint32_t)COSIM_NEGPAD) },
        { "beq",    enc_i(0x04u, 8u, 9u, 1u) },
        { "bne",    enc_i(0x05u, 8u, 9u, 1u) },
        { "blez",   enc_i(0x06u, 8u, 0u, 1u) },
        { "bgtz",   enc_i(0x07u, 8u, 0u, 1u) },
        { "bltz",   enc_i(0x01u, 8u, 0x00u, 1u) },   /* REGIMM */
        { "bgezal", enc_i(0x01u, 8u, 0x11u, 1u) },   /* REGIMM, links */
        { "beql",   enc_i(0x14u, 8u, 9u, 1u) },      /* the likely forms */
        { "bnel",   enc_i(0x15u, 8u, 9u, 1u) },
        { "bc1f",   enc_i(0x11u, 8u, 0u, 1u) },      /* COP1 branch group */
        { "bc2f",   enc_i(0x12u, 8u, 0u, 1u) },      /* COP2 branch group */
        { "jr",     enc_r(8u, 0u, 0u, 0u, 0x08u) },
        { "jalr",   enc_r(8u, 0u, 31u, 0u, 0x09u) },
    };
    const unsigned count = sizeof control / sizeof control[0];
    int failures = 0;

    fprintf(stderr, NEWLINE "cosim: control-in-delay-slot sweep (%u encodings)" NEWLINE,
            count);
    for (unsigned i = 0; i < count; i++) {
        install_lane(0);
        seed_window();
        /* An unconditional `beq $zero, $zero` owns the slot under test, and its
         * taken target is a `break` sentinel. If the slot ever stops being
         * rejected, the run stops AT THE SENTINEL rather than walking the pad --
         * a bounded, attributable failure instead of a hang. */
        MEM_W32(COSIM_NEGPAD + 0u, enc_i(0x04u, 0u, 0u, 1u));
        MEM_W32(COSIM_NEGPAD + 4u, control[i].word);
        MEM_W32(COSIM_NEGPAD + 8u, enc_r(0u, 0u, 0u, 0u, 0x0Du));

        CpuState state;
        CosimCase seed = { "sweep", 0u, 0u, "control sweep", 0u, 0u, NULL };
        seed_state(&state, &seed);

        SrGuestInterpFault fault;
        memset(&fault, 0, sizeof fault);
        s_cpu = &state;
        const SrGuestInterpResult result =
            sr_guest_interp_run(&state, (uint32_t)COSIM_NEGPAD, &fault);
        s_cpu = NULL;

        if (result != SR_GUEST_INTERP_UNSUPPORTED ||
            fault.pc != (uint32_t)COSIM_NEGPAD + 4u ||
            !fault.opcode_valid || fault.opcode != control[i].word) {
            fprintf(stderr,
                    "COSIM SWEEP FAIL %s (0x%08x): result=%s fault.pc=0x%08x "
                    "opcode=0x%08x valid=%d" NEWLINE,
                    control[i].name, control[i].word,
                    sr_guest_interp_result_name(result), fault.pc, fault.opcode,
                    fault.opcode_valid);
            failures++;
        }
    }
    if (failures == 0) {
        fprintf(stderr, "cosim: control sweep OK (%u encodings rejected in a slot)"
                NEWLINE, count);
    }
    return failures;
}

/* `jalr rd, rs` must link into rd, and rd is NOT always $ra.
 *
 * The two-lane comparison cannot reach this: generated code models jal/jalr as a
 * host CALL, so a callee entered through a link register other than $ra cannot
 * return coherently, and no cosim cell can exercise it without faking the frame
 * model. It is still a real decode property of the interpreter, so it is asserted
 * here at the interpreter tier -- which is what closes the gap the two-lane
 * comparison structurally cannot.
 */
static int run_link_register_shape(void) {
    install_lane(0);
    seed_window();
    /* jalr $t1, $t0 with $t0 = the registered trampoline, so the transfer lands
     * on a handoff immediately and the run ends without interpreting further. */
    MEM_W32(COSIM_NEGPAD + 0u, enc_r(8u, 0u, 9u, 0u, 0x09u));
    MEM_W32(COSIM_NEGPAD + 4u, 0u);
    MEM_W32(COSIM_NEGPAD + 8u, enc_r(0u, 0u, 0u, 0u, 0x0Du));   /* break sentinel */

    CpuState state;
    CosimCase seed = { "linkshape", 0u, 0u, "jalr link register", 0u, 0u, NULL };
    seed_state(&state, &seed);
    const uint32_t seeded_ra = state.r[31];
    state.r[8] = (uint32_t)COSIM_RETURN;

    SrGuestInterpFault fault;
    memset(&fault, 0, sizeof fault);
    s_cpu = &state;
    const SrGuestInterpResult result =
        sr_guest_interp_run(&state, (uint32_t)COSIM_NEGPAD, &fault);
    s_cpu = NULL;

    int failures = 0;
    if (result != SR_GUEST_INTERP_AOT_HANDOFF) {
        fprintf(stderr, "COSIM LINKSHAPE FAIL: result=%s" NEWLINE,
                sr_guest_interp_result_name(result));
        failures++;
    }
    if (state.r[9] != (uint32_t)COSIM_NEGPAD + 8u) {
        fprintf(stderr,
                "COSIM LINKSHAPE FAIL: $t1 = 0x%08x, expected the link 0x%08x -- the "
                "rd field of jalr is not being honoured" NEWLINE,
                state.r[9], (uint32_t)COSIM_NEGPAD + 8u);
        failures++;
    }
    if (state.r[31] != seeded_ra) {
        fprintf(stderr,
                "COSIM LINKSHAPE FAIL: $ra = 0x%08x, expected the seed 0x%08x -- the "
                "link was written to $ra rather than to rd" NEWLINE,
                state.r[31], seeded_ra);
        failures++;
    }
    if (failures == 0) {
        fprintf(stderr, "cosim: jalr link-register shape OK ($t1 linked, $ra untouched)"
                NEWLINE);
    }
    return failures;
}

/* ---- behavioral form census ----------------------------------------------------- */
/*
 * "Every form the interpreter implements exists because a cell executes it" was
 * previously enforced from ONE side: a hand-maintained list in
 * tools/test_cosim_fixture.py describing the fixture. Nothing checked the
 * interpreter, so an opcode added with no cell behind it would have gone
 * unnoticed -- which is exactly the speculative coverage the claim rules out.
 *
 * This asks the production interpreter directly instead of parsing it. Each
 * candidate encoding is executed as a real instruction; a form the interpreter
 * does not decode fails closed as SR_GUEST_INTERP_UNSUPPORTED *at that pc*, and
 * anything else means it was decoded (a decoded form may still reject its
 * operands -- a bad address is a memory fault, not an unknown opcode).
 */
typedef enum { FORM_PRIMARY, FORM_SPECIAL, FORM_COP1 } CosimFormKind;

static uint32_t form_probe_word(CosimFormKind kind, uint32_t a, uint32_t b) {
    switch (kind) {
    case FORM_SPECIAL:
        /* Shape 0: the three-operand ALU shape ($t0, $t1 -> $t2).
         * Shape 1: the transfer shape, rt and rd zero.
         *
         * Both are needed. `jr`/`jalr` require the unused fields to be zero and
         * correctly REFUSE shape 0, so probing only that shape would report the
         * fixture's own jr/jalr as undecoded. The question this census asks is
         * "is this form decoded at all", so a form counts as decoded if any
         * well-formed shape of it is accepted. */
        return b == 0u ? enc_r(8u, 9u, 10u, 0u, a) : enc_r(8u, 0u, 0u, 0u, a);
    case FORM_COP1:
        /* fmt in rs, funct in the low bits, ft/fs/fd all f0. */
        return (0x11u << 26) | ((a & 31u) << 21) | (b & 63u);
    default:
        /* rs = $a0 (the seeded scratch pointer) so a load/store form addresses
         * memory that exists and cannot be refused for its operand. */
        return enc_i(a, 4u, 8u, 0u);
    }
}

static int interpreter_decodes(CosimFormKind kind, uint32_t a, uint32_t b) {
    install_lane(0);
    seed_window();
    MEM_W32(COSIM_NEGPAD + 0u, form_probe_word(kind, a, b));
    MEM_W32(COSIM_NEGPAD + 4u, 0u);                       /* benign delay slot */
    MEM_W32(COSIM_NEGPAD + 8u, enc_r(0u, 0u, 0u, 0u, 0x0Du));  /* break: bound the run */

    CpuState state;
    CosimCase seed = { "census", 0u, 0u, "form census", 0u, 0u, NULL };
    seed_state(&state, &seed);
    state.r[4] = COSIM_SCRATCH;

    SrGuestInterpFault fault;
    memset(&fault, 0, sizeof fault);
    s_cpu = &state;
    const SrGuestInterpResult result =
        sr_guest_interp_run(&state, (uint32_t)COSIM_NEGPAD, &fault);
    s_cpu = NULL;

    /* Refused AT the probe word => not decoded. Anything else -- including a
     * fault raised further along, or on this word's operands -- means the form
     * itself was recognised. */
    return !(result == SR_GUEST_INTERP_UNSUPPORTED &&
             fault.pc == (uint32_t)COSIM_NEGPAD);
}

/* A SPECIAL form counts as decoded if either encoding shape is accepted. */
static int form_is_decoded(CosimFormKind kind, uint32_t a, uint32_t b) {
    if (interpreter_decodes(kind, a, b)) {
        return 1;
    }
    return kind == FORM_SPECIAL && interpreter_decodes(kind, a, 1u);
}

typedef struct {
    CosimFormKind kind;
    uint32_t a;
    uint32_t b;
    const char *label;
} CosimForm;

#define COSIM_FORM_ROW(kind_, a_, b_, label_) { FORM_##kind_, a_, b_, label_ },

static int run_form_census(void) {
    static const CosimForm required[] = { COSIM_FORM_LIST(COSIM_FORM_ROW) };
    const unsigned required_count = sizeof required / sizeof required[0];
    int failures = 0;

    fprintf(stderr, NEWLINE "cosim: interpreter form census (%u fixture forms)" NEWLINE,
            required_count);

    /* Direction 1: every form a cell executes must be decoded. */
    for (unsigned i = 0; i < required_count; i++) {
        if (form_is_decoded(required[i].kind, required[i].a, required[i].b)) {
            continue;
        }
        fprintf(stderr,
                "COSIM CENSUS FAIL: the fixture executes %s but the interpreter does "
                "not decode it" NEWLINE, required[i].label);
        failures++;
    }

    /* Direction 2: nothing else is decoded. This is the half that was previously
     * unchecked, and it is what keeps "no speculative opcodes" true. */
    unsigned extra = 0;
    for (uint32_t primary = 0u; primary < 64u; primary++) {
        if (primary == 0x00u || primary == 0x11u) {
            continue;   /* enumerated by their own sub-spaces below */
        }
        if (!interpreter_decodes(FORM_PRIMARY, primary, 0u)) {
            continue;
        }
        int declared = 0;
        for (unsigned i = 0; i < required_count; i++) {
            if (required[i].kind == FORM_PRIMARY && required[i].a == primary) {
                declared = 1;
            }
        }
        if (!declared) {
            fprintf(stderr,
                    "COSIM CENSUS FAIL: the interpreter decodes primary 0x%02x, which "
                    "NO fixture cell executes" NEWLINE, primary);
            extra++;
        }
    }
    for (uint32_t funct = 0u; funct < 64u; funct++) {
        if (!form_is_decoded(FORM_SPECIAL, funct, 0u)) {
            continue;
        }
        int declared = 0;
        for (unsigned i = 0; i < required_count; i++) {
            if (required[i].kind == FORM_SPECIAL && required[i].a == funct) {
                declared = 1;
            }
        }
        if (!declared) {
            fprintf(stderr,
                    "COSIM CENSUS FAIL: the interpreter decodes SPECIAL funct 0x%02x, "
                    "which NO fixture cell executes" NEWLINE, funct);
            extra++;
        }
    }
    for (uint32_t fmt = 0u; fmt < 32u; fmt++) {
        for (uint32_t funct = 0u; funct < 64u; funct++) {
            if (!interpreter_decodes(FORM_COP1, fmt, funct)) {
                continue;
            }
            int declared = 0;
            for (unsigned i = 0; i < required_count; i++) {
                if (required[i].kind == FORM_COP1 && required[i].a == fmt &&
                    required[i].b == funct) {
                    declared = 1;
                }
            }
            if (!declared) {
                fprintf(stderr,
                        "COSIM CENSUS FAIL: the interpreter decodes COP1 fmt=0x%02x "
                        "funct=0x%02x, which NO fixture cell executes" NEWLINE,
                        fmt, funct);
                extra++;
            }
        }
    }

    failures += (int)extra;
    if (failures == 0) {
        fprintf(stderr,
                "cosim: form census OK (%u forms, decoded set == executed set)" NEWLINE,
                required_count);
    }
    return failures;
}

static int run_negative_corpus(void) {
    /* $t0 = 8, $a0 = COSIM_UNOWNED (seeded above). */
    const CosimNegative cases[] = {
        {
            "store-authority", "sr_guest_span_writable() before any store commits",
            /* `break` trails every bounded-access case: if the guard under test
             * ever stops firing, execution stops on the very next word instead of
             * wandering through the pad and reporting some unrelated symptom. */
            { enc_i(0x2Bu, 4u, 8u, 0u), enc_r(0u, 0u, 0u, 0u, 0x0Du) }, 2u, 0u,
            SR_GUEST_INTERP_MEMORY_FAULT, 0u, 1, 0u, (uint32_t)COSIM_UNOWNED, NULL, 0u,
        },
        {
            "load-authority", "sr_guest_span_readable() before any load commits",
            { enc_i(0x23u, 4u, 8u, 0u), enc_r(0u, 0u, 0u, 0u, 0x0Du) }, 2u, 0u,
            SR_GUEST_INTERP_MEMORY_FAULT, 0u, 1, 0u, (uint32_t)COSIM_UNOWNED, NULL, 0u,
        },
        {
            "misaligned-data", "alignment is checked before bounds and before effect",
            /* Inside the window, so ONLY the alignment rule can reject it. */
            { enc_i(0x0Fu, 0u, 4u, (uint32_t)COSIM_SCRATCH >> 16),
              enc_i(0x0Du, 4u, 4u, (uint32_t)COSIM_SCRATCH & 0xffffu),
              enc_i(0x23u, 4u, 8u, 2u) }, 3u, 0u,
            SR_GUEST_INTERP_MISALIGNED_DATA, 8u, 1, 2u,
            (uint32_t)COSIM_SCRATCH + 2u, "r4", (uint32_t)COSIM_SCRATCH,
        },
        {
            "control-in-delay-slot", "a transfer is never decoded as arithmetic",
            /* jal, with a `j` in its delay slot. The link write happens at the
             * transfer and is architecturally correct; the slot is then refused.
             *
             * BOTH transfers name the registered trampoline, never this pad. A
             * negative program has to terminate even when the guard it tests is
             * DELETED -- the interpreter deliberately has no instruction cap, so a
             * transfer back into the pad would turn a removed guard into a hang
             * instead of a failure. (Measured: it did, under `skip-delay-slot`.) */
            { enc_j(0x03u, (uint32_t)COSIM_RETURN), enc_j(0x02u, (uint32_t)COSIM_RETURN) },
            2u, 0u,
            SR_GUEST_INTERP_UNSUPPORTED, 4u, 1, 1u, 0u,
            "r31", (uint32_t)COSIM_NEGPAD + 8u,
        },
        {
            "exec-span-authority", "membership in a registered executable span",
            { 0u }, 0u, NEG_UNOWNED,
            SR_GUEST_INTERP_NOT_EXECUTABLE, 0u, 0, 0u, 0u, NULL, 0u,
        },
        {
            "strict-jr-encoding", "jr with a non-zero hint/rd field is refused",
            { enc_r(8u, 0u, 1u, 0u, 0x08u) }, 1u, 0u,
            SR_GUEST_INTERP_UNSUPPORTED, 0u, 1, 0u, 0u, NULL, 0u,
        },
        {
            "strict-jalr-encoding", "jalr with a non-zero rt field is refused",
            { enc_r(8u, 1u, 31u, 0u, 0x09u) }, 1u, 0u,
            SR_GUEST_INTERP_UNSUPPORTED, 0u, 1, 0u, 0u, NULL, 0u,
        },
        {
            "misaligned-pc", "an unaligned entry is refused before any fetch",
            { 0u }, 0u, 2u,
            SR_GUEST_INTERP_MISALIGNED_PC, 2u, 0, 0u, 0u, NULL, 0u,
        },
    };

    const unsigned count = sizeof cases / sizeof cases[0];
    fprintf(stderr, NEWLINE "cosim: fail-closed negative corpus (%u cases)" NEWLINE, count);
    for (unsigned i = 0; i < count; i++) {
        run_negative(&cases[i]);
    }
    if (g_negative_failures == 0) {
        fprintf(stderr, "cosim: negative corpus OK (%u cases)" NEWLINE, count);
    }
    g_negative_failures += run_control_encoding_sweep();
    g_negative_failures += run_link_register_shape();
    g_negative_failures += run_form_census();
    return g_negative_failures;
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
        fprintf(stderr, "cosim: no comparison cases ran" NEWLINE);
        return 1;
    }

    g_failures += run_negative_corpus();
    fprintf(stderr, "\ncosim: %u comparison cases, %d divergence report(s)\n",
            executed, g_failures);
    if (g_failures != 0) {
        fprintf(stderr, "cosim: FAIL\n");
        return 1;
    }
    fprintf(stderr, "cosim: OK\n");
    return 0;
}
