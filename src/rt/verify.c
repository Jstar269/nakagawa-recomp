// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * Sampled in-play differential checking (issue #1843). Only compiled with -DSR_VERIFY.
 *
 * At a sampled call entry the verifier copies the live guest RAM into the reference
 * interpreter (src/ref, through src/ref/snapshot_ref.h), runs the callee there until it
 * returns or reaches a syscall, and then lets the real translated call run. When the real
 * call returns (sr_aot_return_boundary_end) or reaches the same host boundary
 * (sr_syscall, sr_raw_syscall) the two end states are compared: the stop kind, r1..r31,
 * hi, lo, f0..f31, fcr31, the out-of-range access counts, then RAM and VRAM from the
 * lowest address up. The order and the field names are those of src/rt/snapshot_runner.c,
 * so a captured mismatch replays to the same first difference.
 *
 * The reference runs first on a private copy of RAM, so nothing it does is visible to the
 * guest and the translated call runs on exactly the state the reference saw. One check is
 * in flight at a time; a sampled call that enters while one is in flight is counted as
 * SKIP busy. A guest thread switch, an interrupt or a callback inside the checked call
 * (sr_yield, a return-boundary scope, a stack swap) makes the sample SKIP preempted, and
 * a longjmp-style unwind that drops the call's boundary makes it SKIP abandoned. Every
 * skip is counted by reason in the report and never folded into a match.
 *
 * What is not compared: scratchpad, CpuState fields other than the ones above (VFPU
 * registers, COP0), and anything a guest thread or a device writes to guest memory while
 * the checked call runs on the single guest thread (the report is evidence, not proof).
 * An import stop is matched to the reference's syscall stop at the stub; the import NID is
 * not tied to the stub's syscall instruction.
 */

#ifdef SR_VERIFY

#include "verify.h"
#include "nksnap.h"
#include "../ref/snapshot_ref.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern unsigned long g_oor_reads, g_oor_writes;

typedef enum {
    SKIP_PREEMPTED = 0, SKIP_ABANDONED, SKIP_BUSY, SKIP_RATE, SKIP_NONLOCAL,
    SKIP_CALL_FORM, SKIP_REF_UNIMPLEMENTED, SKIP_REF_NO_VFPU, SKIP_REF_FAULT,
    SKIP_REF_BREAK, SKIP_REF_STEP_LIMIT, SKIP_NO_REF, SKIP_COUNT
} SkipReason;

static const char *const k_skip_names[SKIP_COUNT] = {
    "preempted", "abandoned", "busy", "rate", "nonlocal-return", "unknown-call-form",
    "ref-unimplemented", "ref-no-vfpu", "ref-memory-fault", "ref-break", "ref-step-limit",
    "no-reference",
};

#define CHECKED_AT_MAX 16u

typedef struct {
    uint32_t addr;
    uint64_t calls, checks, match, mismatch, skip;
    uint64_t skip_by[SKIP_COUNT];
    uint64_t checked_at[CHECKED_AT_MAX];
    unsigned checked_at_n;
    int has_first;
    char first_mismatch[112];
} VFn;

typedef struct {
    int live;
    uint64_t serial;
    VFn *fn;
    uint64_t call_no;
    CpuState *state;
    uint32_t target, resume_pc, entry_sp;
    CpuState entry_cpu, ref_cpu;
    NkRefResult rr;
    unsigned long oor_reads0, oor_writes0;
    int preempted;
} Active;

#define VRAM_BASE 0x04000000u
#define VRAM_SIZE 0x00200000u
#define ARENA_PAGE 4096u

static SrVerifyConfig s_cfg;
static int s_inited;
static int s_atexit;
static VFn **s_fns;
static size_t s_fn_count, s_fn_cap;
static uint64_t s_skip_total[SKIP_COUNT];
static uint32_t s_captured;
static uint64_t s_serial;
static Active s_active;
static NkRef *s_ref;
static uint8_t *s_entry_ram;    /* pristine RAM at entry, only when capturing */
static uint8_t *s_vram_base;
static uint64_t (*s_clock)(void);
static double s_tokens;
static uint64_t s_last_ns;
static int s_rate_started;
int sr_verify_suppress_direct;

static uint8_t *arena_base(void) { return g_mem - SR_RAM_BASE; }

static uint64_t default_clock(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void config_error(const char *name, const char *value) {
    fprintf(stderr, "verify: malformed %s=%s\n", name, value);
    exit(2);
}

static uint64_t env_u64(const char *name, int *set, int base) {
    const char *v = getenv(name);
    *set = 0;
    if (!v || !v[0]) return 0;
    char *end = NULL;
    unsigned long long n = strtoull(v, &end, base);
    if (!end || *end != '\0' || end == v) config_error(name, v);
    *set = 1;
    return n;
}

void sr_verify_config_from_env(SrVerifyConfig *cfg) {
    memset(cfg, 0, sizeof *cfg);
    int set = 0;
    uint64_t k = env_u64("SR_VERIFY", &set, 10);
    if (set) { cfg->enabled = 1; if (k > UINT32_MAX) config_error("SR_VERIFY", getenv("SR_VERIFY")); cfg->first_k = (uint32_t)k; }
    uint64_t every = env_u64("SR_VERIFY_EVERY", &set, 10);
    if (set) {
        if (every == 0 || every > UINT32_MAX) config_error("SR_VERIFY_EVERY", getenv("SR_VERIFY_EVERY"));
        cfg->enabled = 1;
        cfg->every = (uint32_t)every;
    }
    const char *rate = getenv("SR_VERIFY_RATE");
    if (rate && rate[0]) {
        char *end = NULL;
        double r = strtod(rate, &end);
        if (!end || *end != '\0' || !(r > 0.0)) config_error("SR_VERIFY_RATE", rate);
        cfg->rate = r;
    }
    const char *range = getenv("SR_VERIFY_RANGE");
    if (range && range[0]) {
        char *end = NULL;
        unsigned long long lo = strtoull(range, &end, 16);
        if (!end || *end != '-' || end == range) config_error("SR_VERIFY_RANGE", range);
        const char *hi_text = end + 1;
        unsigned long long hi = strtoull(hi_text, &end, 16);
        if (!end || *end != '\0' || end == hi_text || hi < lo || hi > UINT32_MAX)
            config_error("SR_VERIFY_RANGE", range);
        cfg->has_range = 1;
        cfg->range_lo = (uint32_t)lo;
        cfg->range_hi = (uint32_t)hi;
    }
    uint64_t steps = env_u64("SR_VERIFY_STEPS", &set, 10);
    cfg->max_steps = set ? steps : 1000000u;
    if (set && steps == 0) config_error("SR_VERIFY_STEPS", getenv("SR_VERIFY_STEPS"));
    const char *p = getenv("SR_VERIFY_REPORT");
    cfg->report_path = (p && p[0]) ? p : NULL;
    p = getenv("SR_VERIFY_CAPTURE");
    cfg->capture_dir = (p && p[0]) ? p : NULL;
    uint64_t cmax = env_u64("SR_VERIFY_CAPTURE_MAX", &set, 10);
    cfg->capture_max = set ? (uint32_t)cmax : 4u;
}

static void free_state(void) {
    for (size_t i = 0; i < s_fn_count; i++) free(s_fns[i]);
    free(s_fns);
    s_fns = NULL;
    s_fn_count = s_fn_cap = 0;
    memset(s_skip_total, 0, sizeof s_skip_total);
    memset(&s_active, 0, sizeof s_active);
    s_captured = 0;
    s_rate_started = 0;
}

static void report_at_exit(void) {
    if (s_cfg.report_path) sr_verify_report_write(s_cfg.report_path);
}

void sr_verify_configure(const SrVerifyConfig *cfg) {
    free_state();
    s_cfg = *cfg;
    if (s_cfg.max_steps == 0) s_cfg.max_steps = 1000000u;
    s_inited = 1;
    if (!s_atexit) { atexit(report_at_exit); s_atexit = 1; }
}

void sr_verify_set_clock(uint64_t (*now_ns)(void)) { s_clock = now_ns; }

static void ensure_init(void) {
    if (s_inited) return;
    SrVerifyConfig cfg;
    sr_verify_config_from_env(&cfg);
    sr_verify_configure(&cfg);
}

/* ---- per-function table ------------------------------------------------------------- */

static VFn *fn_get(uint32_t addr) {
    size_t lo = 0, hi = s_fn_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s_fns[mid]->addr < addr) lo = mid + 1; else hi = mid;
    }
    if (lo < s_fn_count && s_fns[lo]->addr == addr) return s_fns[lo];
    if (s_fn_count == s_fn_cap) {
        size_t cap = s_fn_cap ? s_fn_cap * 2u : 64u;
        VFn **grown = (VFn **)realloc(s_fns, cap * sizeof *grown);
        if (!grown) { fprintf(stderr, "verify: out of memory\n"); exit(2); }
        s_fns = grown;
        s_fn_cap = cap;
    }
    VFn *f = (VFn *)calloc(1u, sizeof *f);
    if (!f) { fprintf(stderr, "verify: out of memory\n"); exit(2); }
    f->addr = addr;
    memmove(&s_fns[lo + 1], &s_fns[lo], (s_fn_count - lo) * sizeof *s_fns);
    s_fns[lo] = f;
    s_fn_count++;
    return f;
}

static void record_skip(VFn *f, SkipReason why) {
    s_skip_total[why]++;
    if (f) { f->skip++; f->skip_by[why]++; }
}

/* ---- sampling ----------------------------------------------------------------------- */

static int sampled(uint64_t n) {
    if (n <= s_cfg.first_k) return 1;
    if (s_cfg.every) return n % s_cfg.every == 0;
    return (n & (n - 1u)) == 0;
}

static int rate_allows(const VFn *f) {
    if (s_cfg.rate <= 0.0) return 1;
    uint64_t (*now)(void) = s_clock ? s_clock : default_clock;
    uint64_t t = now();
    double burst = s_cfg.rate < 1.0 ? 1.0 : s_cfg.rate;
    if (!s_rate_started) {
        s_rate_started = 1;
        s_tokens = burst;
    } else if (t > s_last_ns) {
        s_tokens += (double)(t - s_last_ns) * 1e-9 * s_cfg.rate;
        if (s_tokens > burst) s_tokens = burst;
    }
    s_last_ns = t;
    if (s_tokens >= 1.0 || (f->checks == 0 && s_tokens > -burst)) {
        s_tokens -= 1.0;
        return 1;
    }
    return 0;
}

/* ---- the reference side ------------------------------------------------------------- */

static int call_target(uint32_t call_pc, uint32_t *target) {
    uint32_t phys = call_pc & 0x1FFFFFFFu;
    if (phys < SR_RAM_BASE || phys + 4u > SR_RAM_BASE + NK_REF_RAM_SIZE) return 0;
    uint32_t op;
    memcpy(&op, g_mem + (phys - SR_RAM_BASE), 4u);
    uint32_t major = op >> 26;
    if (major == 3u) {   /* jal */
        *target = ((call_pc + 4u) & 0xF0000000u) | ((op & 0x03FFFFFFu) << 2);
        return 1;
    }
    if (major == 1u) {   /* bltzal, bgezal and their likely forms */
        uint32_t rt = (op >> 16) & 31u;
        if (rt >= 0x10u && rt <= 0x13u) {
            int32_t off = (int16_t)(op & 0xFFFFu);
            *target = call_pc + 4u + (uint32_t)(off * 4);
            return 1;
        }
    }
    return 0;
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

static int ensure_buffers(void) {
    if (!s_ref) s_ref = nk_ref_create();
    if (!s_vram_base) s_vram_base = (uint8_t *)malloc(VRAM_SIZE);
    if (s_cfg.capture_dir && !s_entry_ram) s_entry_ram = (uint8_t *)malloc(NK_REF_RAM_SIZE);
    return s_ref && s_vram_base && (!s_cfg.capture_dir || s_entry_ram);
}

static void start_check(CpuState *s, uint32_t target, uint32_t resume_pc, VFn *f, uint64_t call_no) {
    if (!ensure_buffers()) { record_skip(f, SKIP_NO_REF); return; }
    CpuState start = *s;
    start.r[0] = 0u;
    start.pc = target;
    start.next_pc = 0u;
    start.in_delay_slot = 0u;
    start.flow_kind = 0u;
    start.flow_target = 0u;
    memcpy(nk_ref_ram(s_ref), g_mem, NK_REF_RAM_SIZE);
    if (s_entry_ram) memcpy(s_entry_ram, g_mem, NK_REF_RAM_SIZE);
    memcpy(s_vram_base, arena_base() + VRAM_BASE, VRAM_SIZE);

    Active *a = &s_active;
    memset(a, 0, sizeof *a);
    a->entry_cpu = start;
    a->ref_cpu = start;
    nk_ref_run_call(s_ref, &a->ref_cpu, target, resume_pc, start.r[29], s_cfg.max_steps, &a->rr);
    SkipReason why = SKIP_COUNT;
    switch (a->rr.stop) {
    case NK_REF_UNIMPLEMENTED:
        why = is_vfpu_opcode(a->rr.op) ? SKIP_REF_NO_VFPU : SKIP_REF_UNIMPLEMENTED; break;
    case NK_REF_MEMORY_FAULT: why = SKIP_REF_FAULT; break;
    case NK_REF_BREAK: why = SKIP_REF_BREAK; break;
    case NK_REF_STEP_LIMIT: why = SKIP_REF_STEP_LIMIT; break;
    default: break;
    }
    if (why != SKIP_COUNT) {
        memset(a, 0, sizeof *a);
        record_skip(f, why);
        return;
    }
    a->live = 1;
    a->serial = ++s_serial;
    a->fn = f;
    a->call_no = call_no;
    a->state = s;
    a->target = target;
    a->resume_pc = resume_pc;
    a->entry_sp = start.r[29];
    a->oor_reads0 = g_oor_reads;
    a->oor_writes0 = g_oor_writes;
}

void sr_verify_begin(CpuState *s, uint32_t target, uint32_t call_pc, uint32_t resume_pc) {
    ensure_init();
    if (!s_cfg.enabled) return;
    if (target == 0u && !call_target(call_pc, &target)) {
        /* The callee of this call is unknown, so there is no function to count it under. */
        if (sampled(1u) && !s_active.live) record_skip(NULL, SKIP_CALL_FORM);
        return;
    }
    if (s_cfg.has_range && (target < s_cfg.range_lo || target > s_cfg.range_hi)) return;
    VFn *f = fn_get(target);
    uint64_t n = ++f->calls;
    if (!sampled(n)) return;
    if (s_active.live) { record_skip(f, SKIP_BUSY); return; }
    if (!rate_allows(f)) { record_skip(f, SKIP_RATE); return; }
    start_check(s, target, resume_pc, f, n);
}

/* ---- comparison and verdicts -------------------------------------------------------- */

static uint32_t first_difference(const uint8_t *a, const uint8_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i += 4096u) {
        uint32_t len = n - i < 4096u ? n - i : 4096u;
        if (memcmp(a + i, b + i, len) != 0) {
            for (uint32_t j = 0; j < len; j++)
                if (a[i + j] != b[i + j]) return i + j;
        }
    }
    return n;
}

static int mismatch_hex(char *out, size_t cap, const char *field, uint32_t aot, uint32_t ref) {
    snprintf(out, cap, "%s aot=0x%08" PRIx32 " ref=0x%08" PRIx32, field, aot, ref);
    return 1;
}

/* Same order and wording as src/rt/snapshot_runner.c compare(). */
static int compare(const Active *a, const CpuState *aot, int aot_stop, uint32_t stop_pc,
                   uint32_t stop_code, char *out, size_t cap) {
    static const char *const names[] = { "return", "syscall", "import" };
    const CpuState *r = &a->ref_cpu;
    int ref_stop = a->rr.stop == NK_REF_SYSCALL ? 1 : 0;
    /* An import stub's body ends in a syscall instruction: both are the host boundary. */
    int kinds_agree = aot_stop == ref_stop || (aot_stop == 2 && ref_stop == 1);
    if (!kinds_agree) {
        snprintf(out, cap, "stop aot=%s ref=%s", names[aot_stop], names[ref_stop]);
        return 1;
    }
    if (aot_stop == 1) {
        if (stop_pc != a->rr.pc) return mismatch_hex(out, cap, "syscall_pc", stop_pc, a->rr.pc);
        uint32_t ref_code = (a->rr.op >> 6) & 0xFFFFFu;
        if (stop_code != ref_code) return mismatch_hex(out, cap, "syscall_code", stop_code, ref_code);
    }
    char name[16];
    for (int i = 1; i < 32; i++) {
        if (aot->r[i] != r->r[i]) {
            snprintf(name, sizeof name, "r%d", i);
            return mismatch_hex(out, cap, name, aot->r[i], r->r[i]);
        }
    }
    if (aot->hi != r->hi) return mismatch_hex(out, cap, "hi", aot->hi, r->hi);
    if (aot->lo != r->lo) return mismatch_hex(out, cap, "lo", aot->lo, r->lo);
    for (int i = 0; i < 32; i++) {
        if (aot->fi[i] != r->fi[i]) {
            snprintf(name, sizeof name, "f%d", i);
            return mismatch_hex(out, cap, name, aot->fi[i], r->fi[i]);
        }
    }
    if (aot->fcr31 != r->fcr31) return mismatch_hex(out, cap, "fcr31", aot->fcr31, r->fcr31);
    if (g_oor_reads != a->oor_reads0)
        return mismatch_hex(out, cap, "oor_loads", (uint32_t)(g_oor_reads - a->oor_reads0), 0u);
    if (g_oor_writes != a->oor_writes0)
        return mismatch_hex(out, cap, "oor_stores", (uint32_t)(g_oor_writes - a->oor_writes0), 0u);
    uint32_t low = first_difference(arena_base() + VRAM_BASE, s_vram_base, VRAM_SIZE);
    if (low < VRAM_SIZE) {
        snprintf(out, cap, "mem[0x%08" PRIx32 "] aot=0x%02x ref=0x%02x", VRAM_BASE + low,
                 arena_base()[VRAM_BASE + low], s_vram_base[low]);
        return 1;
    }
    const uint8_t *ref_ram = nk_ref_ram(s_ref);
    uint32_t ram = first_difference(g_mem, ref_ram, NK_REF_RAM_SIZE);
    if (ram < NK_REF_RAM_SIZE) {
        snprintf(out, cap, "mem[0x%08" PRIx32 "] aot=0x%02x ref=0x%02x", SR_RAM_BASE + ram,
                 g_mem[ram], ref_ram[ram]);
        return 1;
    }
    return 0;
}

static void capture(const Active *a, const char *why) {
    if (!s_cfg.capture_dir || !s_entry_ram || s_captured >= s_cfg.capture_max) return;
    /* Every page the call could have depended on or changed: non-zero at entry, or
     * different from the entry bytes on either side afterwards. */
    const uint8_t *ref_ram = nk_ref_ram(s_ref);
    uint32_t pages = NK_REF_RAM_SIZE / ARENA_PAGE;
    NkSnapBlock *blocks = (NkSnapBlock *)calloc(pages / 2u + 1u, sizeof *blocks);
    if (!blocks) return;
    uint32_t count = 0;
    static const uint8_t zero[ARENA_PAGE];
    for (uint32_t p = 0; p < pages; p++) {
        uint32_t off = p * ARENA_PAGE;
        int keep = memcmp(s_entry_ram + off, zero, ARENA_PAGE) != 0 ||
                   memcmp(s_entry_ram + off, ref_ram + off, ARENA_PAGE) != 0 ||
                   memcmp(s_entry_ram + off, g_mem + off, ARENA_PAGE) != 0;
        if (!keep) continue;
        if (count && blocks[count - 1].addr + blocks[count - 1].size == SR_RAM_BASE + off) {
            blocks[count - 1].size += ARENA_PAGE;
        } else {
            blocks[count].addr = SR_RAM_BASE + off;
            blocks[count].size = ARENA_PAGE;
            blocks[count].bytes = s_entry_ram + off;
            count++;
        }
    }
    char path[512], detail[256];
    snprintf(path, sizeof path, "%s/%08" PRIx32 "-call%" PRIu64 ".nksnap", s_cfg.capture_dir,
             a->target, a->call_no);
    NkSnapStatus st = nksnap_write_file(path, &a->entry_cpu, a->target, blocks, count,
                                        detail, sizeof detail);
    if (st == NKSNAP_OK) {
        s_captured++;
        fprintf(stderr, "verify: captured %s (%s)\n", path, why);
    } else {
        fprintf(stderr, "verify: cannot capture %s: %s %s\n", path, nksnap_status_name(st), detail);
    }
    free(blocks);
}

static void finish_skip(SkipReason why) {
    record_skip(s_active.fn, why);
    s_active.live = 0;
}

static void finish(CpuState *s, int aot_stop, uint32_t pc, uint32_t code) {
    Active *a = &s_active;
    if (a->preempted) { finish_skip(SKIP_PREEMPTED); return; }
    char what[sizeof a->fn->first_mismatch];
    int bad = compare(a, s, aot_stop, pc, code, what, sizeof what);
    VFn *f = a->fn;
    f->checks++;
    if (f->checked_at_n < CHECKED_AT_MAX) f->checked_at[f->checked_at_n++] = a->call_no;
    if (bad) {
        f->mismatch++;
        if (!f->has_first) {
            f->has_first = 1;
            snprintf(f->first_mismatch, sizeof f->first_mismatch, "%s", what);
        }
        fprintf(stderr, "verify: MISMATCH 0x%08" PRIx32 " call %" PRIu64 " %s\n", a->target,
                a->call_no, what);
        capture(a, what);
    } else {
        f->match++;
    }
    a->live = 0;
}

void sr_verify_end(CpuState *s, const SrAotReturnBoundary *b) {
    Active *a = &s_active;
    if (!a->live || b->state != a->state || b->resume_pc != a->resume_pc ||
        b->call_sp != a->entry_sp)
        return;
    if (b->unwinding || ((b->observed_target >> 32) != 0u &&
                         (uint32_t)b->observed_target != a->resume_pc)) {
        finish_skip(SKIP_NONLOCAL);
        return;
    }
    finish(s, 0, 0u, 0u);
}

void sr_verify_at_stop(CpuState *s, int kind, uint32_t code, uint32_t pc) {
    Active *a = &s_active;
    if (!a->live || s != a->state) return;
    finish(s, kind == SR_VERIFY_STOP_IMPORT ? 2 : 1, pc, code);
}

void sr_verify_note_preempt(const char *why) {
    (void)why;
    if (s_active.live) s_active.preempted = 1;
}

void sr_verify_abandon(const char *why) {
    (void)why;
    if (s_active.live) finish_skip(SKIP_ABANDONED);
}

/* ---- report ------------------------------------------------------------------------- */

int sr_verify_report_write(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "verify: cannot write %s\n", path); return -1; }
    uint64_t calls = 0, checks = 0, match = 0, mismatch = 0, skip = 0, checked_fns = 0;
    for (size_t i = 0; i < s_fn_count; i++) {
        const VFn *v = s_fns[i];
        calls += v->calls; checks += v->checks; match += v->match;
        mismatch += v->mismatch; skip += v->skip;
        if (v->checks) checked_fns++;
    }
    fprintf(f, "# nk-verify report v1\n");
    fprintf(f, "# functions_entered=%zu functions_checked=%" PRIu64 " calls=%" PRIu64
               " checks=%" PRIu64 " match=%" PRIu64 " mismatch=%" PRIu64 " skip=%" PRIu64 "\n",
            s_fn_count, checked_fns, calls, checks, match, mismatch, skip);
    for (int i = 0; i < SKIP_COUNT; i++)
        if (s_skip_total[i]) fprintf(f, "# skip %s %" PRIu64 "\n", k_skip_names[i], s_skip_total[i]);
    fprintf(f, "# addr calls checks match mismatch skip first_mismatch skips checked_at\n");
    for (size_t i = 0; i < s_fn_count; i++) {
        const VFn *v = s_fns[i];
        fprintf(f, "0x%08" PRIx32 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t",
                v->addr, v->calls, v->checks, v->match, v->mismatch, v->skip);
        if (v->has_first) fprintf(f, "%s\t", v->first_mismatch); else fprintf(f, "-\t");
        int any = 0;
        for (int k = 0; k < SKIP_COUNT; k++) {
            if (!v->skip_by[k]) continue;
            fprintf(f, "%s%s:%" PRIu64, any ? "," : "", k_skip_names[k], v->skip_by[k]);
            any = 1;
        }
        if (!any) fprintf(f, "-");
        fprintf(f, "\t");
        if (!v->checked_at_n) fprintf(f, "-");
        for (unsigned k = 0; k < v->checked_at_n; k++)
            fprintf(f, "%s%" PRIu64, k ? "," : "", v->checked_at[k]);
        if (v->checks > v->checked_at_n) fprintf(f, ",...");   /* the list keeps the first 16 */
        fprintf(f, "\n");
    }
    fclose(f);
    return 0;
}

#endif /* SR_VERIFY */
