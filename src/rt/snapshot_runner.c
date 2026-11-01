// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * Snapshot execution (issue #1842): run one translated guest function from a captured
 * state and check it against the reference interpreter in src/ref.
 *
 *   snapshot_runner [--import ADDR:NID]... [--max-steps N] [--rewrite OUT]
 *                   <image.bin> <base-hex> <snapshot.nksnap>
 *
 * The runner links against one module's generated code, like src/rt/funcdiff.c. It loads
 * the module image at <base>, registers the translated bodies, then overlays the
 * snapshot's memory blocks. From that one starting state it runs
 *
 *   AOT        the translated entry, through production dispatch as a host entry
 *              (dispatch_call_try with SR_HOST_RETURN_PC as $ra and resume PC), until it
 *              returns, reaches its first import (sr_syscall) or its first raw syscall
 *              (sr_raw_syscall). Neither the import handler nor the syscall runs: the
 *              runner's definitions below record where the run stopped and longjmp out.
 *   reference  ref::Run over a copy of the same RAM, until the next fetch is the return
 *              sentinel or a stub named with --import, or ref::Run stops by itself.
 *
 * and prints one verdict line on stdout (tools/nksnap.py pins the grammar):
 *
 *   MATCH
 *   MISMATCH <field> aot=<value> ref=<value>
 *   NOT_CHECKED <reason> [key=value...]
 *   REFUSED <reason> [detail]                  (the snapshot file was refused)
 *
 * Exit status: 0 MATCH, 1 MISMATCH, 2 REFUSED or a usage error, 3 NOT_CHECKED.
 * NOT_CHECKED is never a pass (AGENTS.md section 6).
 *
 * The fields are compared in this order and the first difference is reported: the stop
 * kind (return, syscall, import), the syscall PC and code or the import NID, r1..r31,
 * hi, lo, f0..f31 (raw bits), fcr31, the out-of-range access counts, then memory from the
 * lowest address up.
 *
 * Memory comparison. Both sides start from the same bytes: after the image and the blocks
 * are in the runtime's arena, its 64 MiB of RAM are copied into the reference's RAM, and a
 * baseline copy of the whole arena is kept. After both runs:
 *   - below RAM (scratchpad, VRAM; the reference models neither), any byte the AOT run
 *     changed is a MISMATCH against the unchanged reference view;
 *   - in RAM, the AOT bytes are compared with the reference bytes directly;
 *   - any byte either side changed outside the image and the snapshot blocks makes the
 *     verdict NOT_CHECKED write-outside-snapshot, because the snapshot did not capture
 *     what was there, so the call may have depended on memory it does not hold.
 * Comparing final contents is exact for every byte a run changed. A store that rewrites
 * the value already present changes nothing either side can observe, so it is not a
 * difference. Accesses outside the arena are counted by the runtime (sr_oor) rather than
 * performed; a non-zero count is reported, never dropped.
 *
 * Nested calls go through production dispatch, exactly as in a title build: a callee
 * without a translated body runs on the interpreter floor (src/rt/guest_interp.c).
 */

#include "recomp.h"
#include "nested_frames.h"
#include "nksnap.h"
#include "../ref/snapshot_ref.h"

#include <errno.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sr_register_all(void);   /* generated: executable spans and every native body */

/* The runtime's out-of-range access counters (recomp.c, sr_oor). */
extern unsigned long g_oor_reads, g_oor_writes;

/* ---- the runtime symbols a module and recomp.c reference, for an isolated run ---------- */
/*
 * The runner links the generated module, recomp.c and the dispatch/interpreter units, but
 * no scheduler, HLE, GE or host platform. These definitions stand in for those units. None
 * of them can change a compared result silently: the scheduler is off (sr_sched_on is 0,
 * so SR_YIELD never calls sr_yield), the GE and debug hooks are inert because nothing arms
 * them, and the two calls that would leave guest code -- sr_syscall and sr_raw_syscall --
 * end the AOT run and are reported as its stop.
 */
volatile int g_sr_vram_read_pending;
void sr_ge_vram_read_prepare(uint32_t addr, uint32_t width) { (void)addr; (void)width; }
void sr_ge_vram_write_prepare(uint32_t addr, uint32_t width) { (void)addr; (void)width; }
void sr_ge_vram_write_commit(uint32_t addr, uint32_t width) { (void)addr; (void)width; }

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
void sr_main_module_register(const SrMainModuleRecord *record) { (void)record; }
void sr_yield(CpuState *s) { (void)s; }
uint64_t SDL_GetTicksNS(void) { return 0u; }
/* g_sr_last_writer_enabled stays 0, so the guest accessors never call this. */
void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t val, uint32_t pc) {
    (void)addr; (void)width; (void)val; (void)pc;
}

/* ---- where the AOT run stopped ----------------------------------------------------- */

typedef enum { STOP_RETURN = 0, STOP_SYSCALL, STOP_IMPORT } StopKind;

static const char *const k_stop_names[] = { "return", "syscall", "import" };

static struct {
    StopKind kind;
    uint32_t syscall_pc;
    uint32_t syscall_code;
    uint32_t import_nid;
} s_aot_stop;

static jmp_buf s_stop_jmp;
static int s_aot_running;

/* An import stub's body calls sr_syscall (the generated stub, tools/codegen.py
 * import_stub_text). The handler does not run: the import is where this call ends. */
uint32_t sr_syscall(CpuState *s, uint32_t nid) {
    (void)s;
    if (!s_aot_running) {
        fprintf(stderr, "snapshot_runner: import 0x%08" PRIx32 " outside the AOT run\n", nid);
        abort();
    }
    s_aot_stop.kind = STOP_IMPORT;
    s_aot_stop.import_nid = nid;
    longjmp(s_stop_jmp, 1);
}

/* A raw `syscall` instruction (generated as sr_raw_syscall). recomp.c leaves this symbol to
 * the harness when it is built with SR_GATE_BUILD, as tools/codegen_gate.py does. */
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc) {
    (void)s;
    if (!s_aot_running) {
        fprintf(stderr, "snapshot_runner: raw syscall 0x%" PRIx32 " outside the AOT run\n", code);
        abort();
    }
    s_aot_stop.kind = STOP_SYSCALL;
    s_aot_stop.syscall_code = code;
    s_aot_stop.syscall_pc = pc;
    longjmp(s_stop_jmp, 1);
}

/* ---- command line --------------------------------------------------------------------- */

#define MAX_IMPORTS 256u

typedef struct {
    uint32_t stub[MAX_IMPORTS];
    uint32_t nid[MAX_IMPORTS];
    uint32_t count;
    uint64_t max_steps;
    const char *rewrite;
    const char *image;
    uint32_t base;
    const char *snapshot;
} Options;

static int parse_u32(const char *text, uint32_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(text, &end, 16);
    if (errno || !end || end == text || *end != '\0' || v > UINT32_MAX) return 0;
    *out = (uint32_t)v;
    return 1;
}

static int usage(void) {
    fprintf(stderr,
            "usage: snapshot_runner [--import ADDR:NID]... [--max-steps N] [--rewrite OUT]\n"
            "                       <image.bin> <base-hex> <snapshot.nksnap>\n");
    return 2;
}

static int parse_options(int argc, char **argv, Options *o) {
    memset(o, 0, sizeof *o);
    o->max_steps = 1000000u;
    int positional = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--import") && i + 1 < argc) {
            char buf[64];
            snprintf(buf, sizeof buf, "%s", argv[++i]);
            char *colon = strchr(buf, ':');
            if (!colon || o->count >= MAX_IMPORTS) return 0;
            *colon = '\0';
            if (!parse_u32(buf, &o->stub[o->count]) || !parse_u32(colon + 1, &o->nid[o->count]))
                return 0;
            o->count++;
        } else if (!strcmp(a, "--max-steps") && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            unsigned long long v = strtoull(argv[++i], &end, 10);
            if (errno || !end || *end != '\0' || v == 0u) return 0;
            o->max_steps = v;
        } else if (!strcmp(a, "--rewrite") && i + 1 < argc) {
            o->rewrite = argv[++i];
        } else if (a[0] == '-' && a[1] == '-') {
            return 0;
        } else if (positional == 0) {
            o->image = a; positional++;
        } else if (positional == 1) {
            if (!parse_u32(a, &o->base)) return 0;
            positional++;
        } else if (positional == 2) {
            o->snapshot = a; positional++;
        } else {
            return 0;
        }
    }
    return positional == 3;
}

static uint8_t *read_image(const char *path, uint32_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *data = NULL;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    if (n < 0 || (unsigned long long)n > UINT32_MAX || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    data = (uint8_t *)malloc(n ? (size_t)n : 1u);
    if (data && n && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *len_out = (uint32_t)n;
    return data;
}

/* ---- comparison ------------------------------------------------------------------------ */

#define ARENA_SIZE 0x0c000000u   /* the runtime arena: guest physical [0, 0x0c000000) */

static uint8_t *arena_base(void) { return g_mem - SR_RAM_BASE; }

typedef struct {
    uint32_t *first;
    uint32_t *end;
    uint32_t count;
} Regions;

static int in_regions(const Regions *r, uint32_t addr) {
    for (uint32_t i = 0; i < r->count; i++)
        if (addr >= r->first[i] && addr < r->end[i]) return 1;
    return 0;
}

/* Lowest index in [from, to) where a and b differ, or `to`. */
static uint32_t first_difference(const uint8_t *a, const uint8_t *b, uint32_t from, uint32_t to) {
    const uint32_t step = 4096u;
    uint32_t i = from;
    while (i < to) {
        uint32_t n = to - i < step ? to - i : step;
        if (memcmp(a + i, b + i, n) != 0) {
            for (uint32_t j = 0; j < n; j++)
                if (a[i + j] != b[i + j]) return i + j;
        }
        i += n;
    }
    return to;
}

static int verdict_mismatch_hex(const char *field, uint32_t aot, uint32_t ref) {
    printf("MISMATCH %s aot=0x%08" PRIx32 " ref=0x%08" PRIx32 "\n", field, aot, ref);
    return 1;
}

static int verdict_mismatch_mem(uint32_t addr, uint8_t aot, uint8_t ref) {
    printf("MISMATCH mem[0x%08" PRIx32 "] aot=0x%02x ref=0x%02x\n", addr, aot, ref);
    return 1;
}

static int is_vfpu_opcode(uint32_t op) {
    switch (op >> 26) {
    case 0x12: case 0x18: case 0x19: case 0x1b: case 0x32: case 0x34: case 0x35:
    case 0x36: case 0x37: case 0x3a: case 0x3c: case 0x3d: case 0x3e: case 0x3f:
        return 1;
    default:
        return 0;
    }
}

/* NOT_CHECKED for a reference stop that is not a comparable end of the call. */
static int verdict_reference_stop(const NkRefResult *rr, uint64_t max_steps) {
    switch (rr->stop) {
    case NK_REF_UNIMPLEMENTED:
        printf("NOT_CHECKED %s pc=0x%08" PRIx32 " op=0x%08" PRIx32 "\n",
               is_vfpu_opcode(rr->op) ? "ref-no-vfpu" : "ref-unimplemented", rr->pc, rr->op);
        return 3;
    case NK_REF_MEMORY_FAULT:
        printf("NOT_CHECKED ref-memory-fault pc=0x%08" PRIx32 " op=0x%08" PRIx32
               " addr=0x%08" PRIx32 "\n", rr->pc, rr->op, rr->fault_addr);
        return 3;
    case NK_REF_BREAK:
        printf("NOT_CHECKED ref-break pc=0x%08" PRIx32 " op=0x%08" PRIx32 "\n", rr->pc, rr->op);
        return 3;
    case NK_REF_STEP_LIMIT:
        printf("NOT_CHECKED ref-step-limit steps=%" PRIu64 "\n", max_steps);
        return 3;
    case NK_REF_RETURNED:
    case NK_REF_IMPORT:
    case NK_REF_SYSCALL:
        break;
    }
    return 0;
}

static StopKind ref_stop_kind(const NkRefResult *rr) {
    return rr->stop == NK_REF_IMPORT ? STOP_IMPORT
         : rr->stop == NK_REF_SYSCALL ? STOP_SYSCALL : STOP_RETURN;
}

static CpuState g_aot_cpu;     /* static: a longjmp out of guest code must not lose it */
static CpuState g_ref_cpu;

static int compare(const Options *o, const NkRefResult *rr, const uint8_t *ref_ram,
                   const uint8_t *baseline, const Regions *known,
                   unsigned long oor_reads, unsigned long oor_writes) {
    const CpuState *a = &g_aot_cpu, *r = &g_ref_cpu;
    StopKind ref_kind = ref_stop_kind(rr);
    if (s_aot_stop.kind != ref_kind) {
        printf("MISMATCH stop aot=%s ref=%s\n", k_stop_names[s_aot_stop.kind],
               k_stop_names[ref_kind]);
        return 1;
    }
    if (ref_kind == STOP_SYSCALL) {
        if (s_aot_stop.syscall_pc != rr->pc)
            return verdict_mismatch_hex("syscall_pc", s_aot_stop.syscall_pc, rr->pc);
        uint32_t ref_code = (rr->op >> 6) & 0xFFFFFu;
        if (s_aot_stop.syscall_code != ref_code)
            return verdict_mismatch_hex("syscall_code", s_aot_stop.syscall_code, ref_code);
    } else if (ref_kind == STOP_IMPORT) {
        uint32_t ref_nid = 0;
        for (uint32_t i = 0; i < o->count; i++)
            if (o->stub[i] == rr->pc) { ref_nid = o->nid[i]; break; }
        if (s_aot_stop.import_nid != ref_nid)
            return verdict_mismatch_hex("import_nid", s_aot_stop.import_nid, ref_nid);
    }
    char name[16];
    for (int i = 1; i < 32; i++) {
        if (a->r[i] != r->r[i]) {
            snprintf(name, sizeof name, "r%d", i);
            return verdict_mismatch_hex(name, a->r[i], r->r[i]);
        }
    }
    if (a->hi != r->hi) return verdict_mismatch_hex("hi", a->hi, r->hi);
    if (a->lo != r->lo) return verdict_mismatch_hex("lo", a->lo, r->lo);
    for (int i = 0; i < 32; i++) {
        if (a->fi[i] != r->fi[i]) {
            snprintf(name, sizeof name, "f%d", i);
            return verdict_mismatch_hex(name, a->fi[i], r->fi[i]);
        }
    }
    if (a->fcr31 != r->fcr31) return verdict_mismatch_hex("fcr31", a->fcr31, r->fcr31);
    /* The reference ran to a comparable stop, so it made no access outside its RAM. */
    if (oor_reads != 0u) return verdict_mismatch_hex("oor_loads", (uint32_t)oor_reads, 0u);
    if (oor_writes != 0u) return verdict_mismatch_hex("oor_stores", (uint32_t)oor_writes, 0u);

    const uint8_t *arena = arena_base();
    uint32_t low = first_difference(arena, baseline, 0u, SR_RAM_BASE);
    if (low < SR_RAM_BASE) return verdict_mismatch_mem(low, arena[low], baseline[low]);
    uint32_t ram = first_difference(arena + SR_RAM_BASE, ref_ram, 0u, NK_REF_RAM_SIZE);
    if (ram < NK_REF_RAM_SIZE)
        return verdict_mismatch_mem(SR_RAM_BASE + ram, arena[SR_RAM_BASE + ram], ref_ram[ram]);

    /* Both sides now hold identical bytes everywhere, so the AOT arena speaks for both. */
    uint32_t from = 0u;
    for (;;) {
        uint32_t at = first_difference(arena, baseline, from, ARENA_SIZE);
        if (at >= ARENA_SIZE) break;
        if (!in_regions(known, at)) {
            printf("NOT_CHECKED write-outside-snapshot addr=0x%08" PRIx32 " side=both\n", at);
            return 3;
        }
        from = at + 1u;
    }
    printf("MATCH\n");
    return 0;
}

int main(int argc, char **argv) {
    Options o;
    if (!parse_options(argc, argv, &o)) return usage();

    NkSnapshot snap;
    char detail[256];
    NkSnapStatus st = nksnap_load_file(o.snapshot, &snap, detail, sizeof detail);
    if (st != NKSNAP_OK) {
        printf("REFUSED %s%s%s\n", nksnap_status_name(st), detail[0] ? " " : "", detail);
        return 2;
    }
    if (o.rewrite) {
        st = nksnap_write_file(o.rewrite, &snap.cpu, snap.entry_pc, snap.blocks,
                               snap.block_count, detail, sizeof detail);
        if (st != NKSNAP_OK) {
            fprintf(stderr, "snapshot_runner: cannot rewrite %s: %s %s\n", o.rewrite,
                    nksnap_status_name(st), detail);
            return 2;
        }
    }
    for (uint32_t i = 0; i < snap.block_count; i++) {
        const NkSnapBlock *b = &snap.blocks[i];
        if (b->addr < NK_REF_RAM_BASE || (uint64_t)b->addr + b->size > (uint64_t)NK_REF_RAM_BASE + NK_REF_RAM_SIZE) {
            printf("NOT_CHECKED block-outside-ref-ram addr=0x%08" PRIx32 " size=0x%" PRIx32 "\n",
                   b->addr, b->size);
            return 3;
        }
    }

    uint32_t image_len = 0;
    uint8_t *image = read_image(o.image, &image_len);
    if (!image) {
        fprintf(stderr, "snapshot_runner: cannot read %s\n", o.image);
        return 2;
    }
    sr_mem_init();
    sr_load_segment(o.base, image, image_len);
    free(image);
    sr_register_all();
    RecompFn fn = sr_lookup(snap.entry_pc);
    if (!fn) {
        printf("NOT_CHECKED aot-no-body pc=0x%08" PRIx32 "\n", snap.entry_pc);
        return 3;
    }
    for (uint32_t i = 0; i < snap.block_count; i++)
        memcpy(arena_base() + snap.blocks[i].addr, snap.blocks[i].bytes, snap.blocks[i].size);

    /* The bytes the snapshot accounts for: the module image and every block. */
    Regions known = { NULL, NULL, 0u };
    known.first = (uint32_t *)calloc((size_t)snap.block_count + 1u, sizeof *known.first);
    known.end = (uint32_t *)calloc((size_t)snap.block_count + 1u, sizeof *known.end);
    if (!known.first || !known.end) {
        fprintf(stderr, "snapshot_runner: out of memory\n");
        return 2;
    }
    known.first[known.count] = o.base;
    known.end[known.count++] = o.base + image_len;
    for (uint32_t i = 0; i < snap.block_count; i++) {
        known.first[known.count] = snap.blocks[i].addr;
        known.end[known.count++] = snap.blocks[i].addr + snap.blocks[i].size;
    }

    /* The call starts at its entry: $ra is the host-return sentinel on both sides and no
     * transfer is pending. Every other field is the snapshot's. */
    CpuState start = snap.cpu;
    start.r[0] = 0u;
    start.r[31] = SR_HOST_RETURN_PC;
    start.pc = snap.entry_pc;
    start.next_pc = 0u;
    start.in_delay_slot = 0u;
    start.flow_kind = 0u;
    start.flow_target = 0u;

    uint8_t *baseline = (uint8_t *)malloc(ARENA_SIZE);
    NkRef *ref = nk_ref_create();
    if (!baseline || !ref) {
        fprintf(stderr, "snapshot_runner: out of memory\n");
        return 2;
    }
    memcpy(baseline, arena_base(), ARENA_SIZE);
    memcpy(nk_ref_ram(ref), g_mem, NK_REF_RAM_SIZE);

    /* Reference first: a run it cannot finish (step budget) is not worth an AOT run that
     * may not finish either, and its verdict is NOT_CHECKED regardless. */
    NkRefResult rr;
    g_ref_cpu = start;
    nk_ref_run(ref, &g_ref_cpu, snap.entry_pc, SR_HOST_RETURN_PC, o.stub, o.count,
               o.max_steps, &rr);
    static const char *const ref_stop_names[] = {
        "return", "import", "syscall", "unimplemented", "memory-fault", "break", "step-limit",
    };
    fprintf(stderr, "reference: stop=%s pc=0x%08" PRIx32 " op=0x%08" PRIx32 " executed=%" PRIu64 "\n",
            ref_stop_names[rr.stop], rr.pc, rr.op, rr.executed);
    int code = verdict_reference_stop(&rr, o.max_steps);
    if (code != 0) {
        nk_ref_destroy(ref);
        free(baseline);
        nksnap_free(&snap);
        return code;
    }

    g_aot_cpu = start;
    memset(&s_aot_stop, 0, sizeof s_aot_stop);
    unsigned long oor_reads = g_oor_reads, oor_writes = g_oor_writes;
    int dispatch_rc = 0;
    s_aot_running = 1;
    if (setjmp(s_stop_jmp) == 0) {
        dispatch_rc = dispatch_call_try(&g_aot_cpu, snap.entry_pc, SR_HOST_RETURN_PC);
    } else {
        /* The stop longjmped out of every native body in flight: discard their AOT
         * return boundaries and nested guest-call frames, as src/rt/funcdiff.c does. */
        sr_aot_return_boundaries_reset();
        sr_tail_assert_empty("tail-transfer-live-at-snapshot-stop");
        sr_nested_frame_release_all();
    }
    s_aot_running = 0;
    oor_reads = g_oor_reads - oor_reads;
    oor_writes = g_oor_writes - oor_writes;
    fprintf(stderr, "aot: stop=%s nid=0x%08" PRIx32 " syscall=0x%" PRIx32 "@0x%08" PRIx32 "\n",
            k_stop_names[s_aot_stop.kind], s_aot_stop.import_nid, s_aot_stop.syscall_code,
            s_aot_stop.syscall_pc);
    if (dispatch_rc < 0) {
        printf("NOT_CHECKED aot-dispatch-error code=%d\n", dispatch_rc);
        code = 3;
    } else {
        code = compare(&o, &rr, nk_ref_ram(ref), baseline, &known, oor_reads, oor_writes);
    }
    nk_ref_destroy(ref);
    free(baseline);
    nksnap_free(&snap);
    return code;
}
