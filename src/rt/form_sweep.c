// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * Form sweep (issue #1844): run every instance of every scalar instruction form on the
 * translated body and on the reference interpreter from the same seeded states, and
 * compare the architectural results.
 *
 *   form_sweep [--only INDEX] [--dump STATE] <image.bin> <base-hex> <plan.txt>
 *
 * tools/form_sweep.py writes the plan: a seed, a state count, the scratch window and one
 * line per instance (`inst <index> <entry> <word> <base reg> <offset> <gpr mask> <fpr mask>`;
 * every number is hexadecimal except the index and the base register). Each instance is a
 * tiny guest function, so the translated body is the real code generator's output for
 * that word. For each state the harness
 *
 *   - draws the register file from edge values (zeros, ones, sign boundaries, a scratch
 *     pointer, random bits) and floats from special values (signed zeros, denormals,
 *     infinities, NaNs, FLT_MAX) 30% of the time, with a random FCR31 rounding mode;
 *   - on odd states makes the registers the instance names pairwise distinct, so an
 *     aliasing defect cannot hide behind equal values;
 *   - when two named float registers hold NaNs with different payloads, makes them equal:
 *     an arithmetic result then carries whichever payload the host compiler's operand order
 *     picks inside the shared helper, and the PSP's choice is unmeasured (counted on stderr);
 *   - for a memory form points the base register so base + offset falls inside the
 *     scratch window, and fills the window with random bytes on both sides;
 *   - runs the translated body through production dispatch (as snapshot_runner.c does)
 *     and ref::Run, then compares r1..r31, hi, lo, f0..f31 (raw bits), fcr31, fpcond,
 *     llbit, the scratch window and the runtime's out-of-range access counters.
 *
 * Every state is a pure function of (seed, instance, state), so a failing line replays
 * with `--only INDEX`. One verdict line per instance goes to stdout:
 *
 *   INST <index> MATCH states=<n>
 *   INST <index> MISMATCH states=<n> bad=<k> first=<state> <field> aot=<v> ref=<v>
 *   INST <index> NOT_CHECKED <reason> [key=value...]
 *
 * NOT_CHECKED is never a pass: the reference stopped on the instruction (unimplemented,
 * fault, break) or the translated body has no entry. Exit status: 0 when no instance
 * mismatched, 1 otherwise, 2 for a usage or input error.
 *
 * The harness links snapshot_runner.c for its stand-ins of the scheduler, GE and host
 * symbols (see that file) and reuses its stop handling; only main is renamed.
 */

#define main snapshot_runner_main
#include "snapshot_runner.c"
#undef main

typedef struct {
    uint32_t index;
    uint32_t entry;
    uint32_t word;
    int32_t base_reg;
    uint32_t offset;
    uint32_t gmask;
    uint32_t fmask;
} SweepInst;

static uint32_t g_nan_ties;   /* states changed by the NaN-payload rule in make_state */

static uint64_t rng_next(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static const uint32_t k_float_special[] = {
    0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x40000000u, 0x3F000000u, 0x7F800000u,
    0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0x7FA00000u, 0x00000001u, 0x807FFFFFu,
    0x007FFFFFu, 0x00800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x4B800000u, 0x4F000000u, 0xCF000000u,
    0x4EFFFFFFu, 0x3F000001u, 0x3EFFFFFFu, 0x4B7FFFFFu, 0x40490FDBu,
};

static uint32_t draw_gpr(uint64_t *rng, uint32_t pointer) {
    uint64_t r = rng_next(rng);
    uint32_t pick = (uint32_t)(r % 20u);
    uint32_t bits = (uint32_t)(r >> 32);
    switch (pick) {
    case 0: case 1: return 0u;
    case 2: return 1u;
    case 3: return 2u;
    case 4: return 0xFFFFFFFFu;
    case 5: return 0x80000000u;
    case 6: return 0x7FFFFFFFu;
    case 7: return 0x0000FFFFu;
    case 8: return 0xFFFF8000u;
    case 9: return bits & 0xFFu;
    case 10: return (uint32_t)(int32_t)(int8_t)(bits & 0xFFu);
    case 11: return pointer;
    case 12: return 31u;
    case 13: return 32u;
    default: return bits;
    }
}

static uint32_t draw_fpr(uint64_t *rng) {
    uint64_t r = rng_next(rng);
    if (r % 100u < 30u)
        return k_float_special[(r >> 8) % (sizeof k_float_special / sizeof k_float_special[0])];
    return (uint32_t)(r >> 32);
}

static void make_state(CpuState *cpu, const SweepInst *in, uint32_t state_index, uint64_t seed,
                       uint32_t scratch, uint32_t scratch_size) {
    uint64_t rng = seed ^ ((uint64_t)in->index * 0xD6E8FEB86659FD93ull)
                        ^ ((uint64_t)state_index * 0xA0761D6478BD642Full);
    memset(cpu, 0, sizeof *cpu);
    uint32_t pointer = scratch + scratch_size / 2u;
    for (int i = 1; i < 32; i++) cpu->r[i] = draw_gpr(&rng, pointer);
    for (int i = 0; i < 32; i++) cpu->fi[i] = draw_fpr(&rng);
    cpu->hi = draw_gpr(&rng, pointer);
    cpu->lo = draw_gpr(&rng, pointer);
    uint32_t fcr = (uint32_t)(rng_next(&rng) >> 40);
    cpu->fcr31 = (fcr & 3u) | (((fcr >> 2) & 1u) << 23) | (((fcr >> 3) % 4u == 0u) ? 1u << 24 : 0u);
    cpu->fpcond = (cpu->fcr31 >> 23) & 1u;
    cpu->llbit = (uint32_t)(rng_next(&rng) & 1u);
    if (state_index & 1u) {
        /* Odd states: every named register holds a different value. */
        uint32_t seen[32];
        int n = 0;
        for (int i = 1; i < 32; i++) {
            if (!((in->gmask >> i) & 1u)) continue;
            for (int tries = 0; tries < 64; tries++) {
                int clash = 0;
                for (int k = 0; k < n; k++) clash |= (seen[k] == cpu->r[i]);
                if (!clash) break;
                cpu->r[i] = (uint32_t)rng_next(&rng);
            }
            seen[n++] = cpu->r[i];
        }
        n = 0;
        for (int i = 0; i < 32; i++) {
            if (!((in->fmask >> i) & 1u)) continue;
            for (int tries = 0; tries < 64; tries++) {
                int clash = 0;
                for (int k = 0; k < n; k++) clash |= (seen[k] == cpu->fi[i]);
                if (!clash) break;
                cpu->fi[i] = draw_fpr(&rng);
            }
            seen[n++] = cpu->fi[i];
        }
    }
    /* Two named float registers that both hold a NaN, with different payloads: which payload
     * an arithmetic result carries depends on the operand order the host compiler chose in
     * the shared helper (src/rt/fp_convert.h), and the PSP's choice is unmeasured. The
     * translated body and the reference are built with different flags, so they can pick
     * differently and neither is wrong. Make such a pair equal and count it, so the case is
     * visible in the summary instead of a mismatch no source change can settle. */
    {
        int first = -1;
        for (int i = 0; i < 32; i++) {
            if (!((in->fmask >> i) & 1u) || (cpu->fi[i] & 0x7FFFFFFFu) <= 0x7F800000u) continue;
            if (first < 0) {
                first = i;
            } else if (cpu->fi[i] != cpu->fi[first]) {
                cpu->fi[i] = cpu->fi[first];
                g_nan_ties++;
            }
        }
    }
    if (in->base_reg > 0) {
        /* The effective address lands in the middle half of the window, any alignment. */
        uint32_t target = scratch + scratch_size / 4u + (uint32_t)(rng_next(&rng) % (scratch_size / 2u));
        cpu->r[in->base_reg] = target - in->offset;
    }
    cpu->r[0] = 0u;
    cpu->r[31] = SR_HOST_RETURN_PC;
}

typedef struct {
    int bad;
    char field[24];
    uint32_t aot;
    uint32_t ref;
} Diff;

static void diff_field(Diff *d, const char *name, uint32_t aot, uint32_t ref) {
    if (d->bad || aot == ref) return;
    d->bad = 1;
    snprintf(d->field, sizeof d->field, "%s", name);
    d->aot = aot;
    d->ref = ref;
}

static void compare_states(Diff *d, const CpuState *a, const CpuState *r) {
    char name[24];
    for (int i = 1; i < 32 && !d->bad; i++) {
        snprintf(name, sizeof name, "r%d", i);
        diff_field(d, name, a->r[i], r->r[i]);
    }
    diff_field(d, "hi", a->hi, r->hi);
    diff_field(d, "lo", a->lo, r->lo);
    for (int i = 0; i < 32 && !d->bad; i++) {
        snprintf(name, sizeof name, "f%d", i);
        diff_field(d, name, a->fi[i], r->fi[i]);
    }
    diff_field(d, "fcr31", a->fcr31, r->fcr31);
    diff_field(d, "fpcond", a->fpcond, r->fpcond);
    diff_field(d, "llbit", a->llbit, r->llbit);
}

static int parse_plan(const char *path, uint64_t *seed, uint32_t *states, uint32_t *scratch,
                      uint32_t *scratch_size, SweepInst **out, uint32_t *count) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    uint32_t cap = 256u, n = 0u;
    SweepInst *v = (SweepInst *)malloc(cap * sizeof *v);
    char line[256];
    *seed = 0u;
    *states = 0u;
    *scratch = 0u;
    *scratch_size = 0u;
    while (v && fgets(line, sizeof line, f)) {
        unsigned long long a;
        unsigned b, c, d2, e, g, h;
        int base;
        if (sscanf(line, "seed %llu", &a) == 1) {
            *seed = a;
        } else if (sscanf(line, "states %u", &b) == 1) {
            *states = b;
        } else if (sscanf(line, "scratch %x %x", &b, &c) == 2) {
            *scratch = b;
            *scratch_size = c;
        } else if (sscanf(line, "inst %u %x %x %d %x %x %x", &b, &c, &d2, &base, &e, &g, &h) == 7) {
            if (n == cap) {
                cap *= 2u;
                v = (SweepInst *)realloc(v, cap * sizeof *v);
                if (!v) break;
            }
            v[n].index = b;
            v[n].entry = c;
            v[n].word = d2;
            v[n].base_reg = base;
            v[n].offset = e;
            v[n].gmask = g;
            v[n].fmask = h;
            n++;
        }
    }
    fclose(f);
    if (!v) return 0;
    *out = v;
    *count = n;
    return *states != 0u && *scratch_size >= 0x40u && n != 0u;
}

int main(int argc, char **argv) {
    long only = -1;
    long dump_state = -1;
    int positional = 0;
    const char *image_path = NULL, *plan_path = NULL;
    uint32_t base = 0u;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--only") && i + 1 < argc) {
            only = strtol(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
            dump_state = strtol(argv[++i], NULL, 10);
        } else if (positional == 0) {
            image_path = argv[i]; positional++;
        } else if (positional == 1) {
            if (!parse_u32(argv[i], &base)) return 2;
            positional++;
        } else if (positional == 2) {
            plan_path = argv[i]; positional++;
        } else {
            positional = 99;
        }
    }
    if (positional != 3) {
        fprintf(stderr, "usage: form_sweep [--only INDEX] [--dump STATE] <image.bin> <base-hex> <plan.txt>\n");
        return 2;
    }
    uint64_t seed;
    uint32_t states, scratch, scratch_size, count = 0u;
    SweepInst *insts = NULL;
    if (!parse_plan(plan_path, &seed, &states, &scratch, &scratch_size, &insts, &count)) {
        fprintf(stderr, "form_sweep: cannot read a plan from %s\n", plan_path);
        return 2;
    }
    uint32_t image_len = 0u;
    uint8_t *image = read_image(image_path, &image_len);
    if (!image) {
        fprintf(stderr, "form_sweep: cannot read %s\n", image_path);
        return 2;
    }
    sr_mem_init();
    sr_load_segment(base, image, image_len);
    free(image);
    sr_register_all();
    NkRef *ref = nk_ref_create();
    if (!ref) {
        fprintf(stderr, "form_sweep: out of memory\n");
        return 2;
    }
    uint8_t *aot_window = arena_base() + scratch;
    uint8_t *ref_window = nk_ref_ram(ref) + (scratch - NK_REF_RAM_BASE);
    /* The image is not part of what an instruction changes, but the reference needs it to
     * fetch: copy the RAM the runtime holds so both sides run the same instruction bytes. */
    memcpy(nk_ref_ram(ref), g_mem, NK_REF_RAM_SIZE);

    uint32_t mismatched = 0u;
    for (uint32_t k = 0; k < count; k++) {
        const SweepInst *in = &insts[k];
        if (only >= 0 && (uint32_t)only != in->index) continue;
        RecompFn fn = sr_lookup(in->entry);
        if (!fn) {
            printf("INST %u NOT_CHECKED aot-no-body pc=0x%08x op=0x%08x\n", in->index, in->entry, in->word);
            continue;
        }
        uint32_t bad_states = 0u, first_state = 0u;
        Diff first;
        memset(&first, 0, sizeof first);
        int not_checked = 0;
        for (uint32_t st = 0; st < states && !not_checked; st++) {
            CpuState start;
            make_state(&start, in, st, seed, scratch, scratch_size);
            start.pc = in->entry;
            if (dump_state >= 0 && (uint32_t)dump_state == st && only >= 0) {
                fprintf(stderr, "state %u of instance %u (op 0x%08x):\n", st, in->index, in->word);
                for (int i = 1; i < 32; i++) fprintf(stderr, " r%d=0x%08x", i, start.r[i]);
                fprintf(stderr, "\n hi=0x%08x lo=0x%08x fcr31=0x%08x llbit=%u\n", start.hi, start.lo,
                        start.fcr31, start.llbit);
                for (int i = 0; i < 32; i++) fprintf(stderr, " f%d=0x%08x", i, start.fi[i]);
                fprintf(stderr, "\n");
            }
            uint64_t wr = seed ^ 0x5CA77E4Bull ^ ((uint64_t)in->index << 20) ^ st;
            for (uint32_t i = 0; i < scratch_size; i++) {
                uint8_t b = (uint8_t)rng_next(&wr);
                aot_window[i] = b;
                ref_window[i] = b;
            }

            NkRefResult rr;
            g_ref_cpu = start;
            nk_ref_run(ref, &g_ref_cpu, in->entry, SR_HOST_RETURN_PC, NULL, 0u, 64u, &rr);
            if (rr.stop != NK_REF_RETURNED) {
                static const char *const names[] = {
                    "return", "import", "syscall", "ref-unimplemented", "ref-memory-fault",
                    "ref-break", "ref-step-limit",
                };
                printf("INST %u NOT_CHECKED %s pc=0x%08x op=0x%08x state=%u\n", in->index,
                       names[rr.stop], rr.pc, rr.op, st);
                not_checked = 1;
                break;
            }

            g_aot_cpu = start;
            memset(&s_aot_stop, 0, sizeof s_aot_stop);
            unsigned long oor_reads = g_oor_reads, oor_writes = g_oor_writes;
            int dispatch_rc = 0;
            s_aot_running = 1;
            if (setjmp(s_stop_jmp) == 0) {
                dispatch_rc = dispatch_call_try(&g_aot_cpu, in->entry, SR_HOST_RETURN_PC);
            } else {
                sr_aot_return_boundaries_reset();
                sr_tail_assert_empty("tail-transfer-live-at-form-sweep-stop");
                sr_nested_frame_release_all();
                dispatch_rc = -1000;
            }
            s_aot_running = 0;
            oor_reads = g_oor_reads - oor_reads;
            oor_writes = g_oor_writes - oor_writes;
            if (dispatch_rc < 0) {
                printf("INST %u NOT_CHECKED aot-dispatch-error code=%d op=0x%08x state=%u\n", in->index,
                       dispatch_rc, in->word, st);
                not_checked = 1;
                break;
            }
            Diff d;
            memset(&d, 0, sizeof d);
            compare_states(&d, &g_aot_cpu, &g_ref_cpu);
            if (!d.bad) {
                uint32_t at = 0u;
                while (at < scratch_size && aot_window[at] == ref_window[at]) at++;
                if (at < scratch_size) {
                    d.bad = 1;
                    snprintf(d.field, sizeof d.field, "mem[+0x%x]", at);
                    d.aot = aot_window[at];
                    d.ref = ref_window[at];
                }
            }
            if (!d.bad && oor_reads) diff_field(&d, "oor_loads", (uint32_t)oor_reads, 0u);
            if (!d.bad && oor_writes) diff_field(&d, "oor_stores", (uint32_t)oor_writes, 0u);
            if (d.bad) {
                if (bad_states == 0u) {
                    first = d;
                    first_state = st;
                }
                bad_states++;
            }
        }
        if (not_checked) continue;
        if (bad_states) {
            printf("INST %u MISMATCH states=%u bad=%u first=%u %s aot=0x%08x ref=0x%08x\n", in->index,
                   states, bad_states, first_state, first.field, first.aot, first.ref);
            mismatched++;
        } else {
            printf("INST %u MATCH states=%u\n", in->index, states);
        }
    }
    fprintf(stderr, "form_sweep: %u float register pairs held NaNs with different payloads and were "
            "made equal (operand-order-dependent result, PSP choice unmeasured)\n", g_nan_ties);
    nk_ref_destroy(ref);
    free(insts);
    return mismatched ? 1 : 0;
}
