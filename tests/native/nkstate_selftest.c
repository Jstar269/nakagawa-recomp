// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * nkstate selftest (#2146, #2147): the state container round trip and every refusal.
 *
 * The round trip runs real guest code through production dispatch (dispatch_call_try into
 * an analyzer-owned span with no translated body, so the interpreter floor runs it). The
 * guest is a small decoder with history: each call ("frame") advances an LCG, folds it into
 * a filter whose history lives in guest RAM, writes 16 output words, and keeps a running
 * checksum in $v1 across calls, so its next frame depends on both registers and memory.
 *
 *   uninterrupted   2N frames from a fixed start;
 *   interrupted     N frames, capture to a file, clobber the registers and the data
 *                   window, restore from the file, N more frames.
 *
 * The per-frame trace, the final CpuState and the final data window must be identical.
 * A control continues from the clobbered state without restoring and must differ, so the
 * comparison is not vacuous. No game inputs are needed.
 */

#include "recomp.c"   /* arena, dispatch table, dispatch_call_try */
#include "cpu_lle.c"  /* flow helpers the linked interpreter needs */

#include "nkstate.h"
#include "replay_checkpoint.h"
#include "vfpu_tables.h"

/* No GE renderer is linked by this harness. */
volatile int g_sr_vram_read_pending;
void sr_ge_vram_read_prepare(uint32_t addr, uint32_t width) { (void)addr; (void)width; }
void sr_ge_vram_write_prepare(uint32_t addr, uint32_t width) { (void)addr; (void)width; }
void sr_ge_vram_write_commit(uint32_t addr, uint32_t width) { (void)addr; (void)width; }

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

/* The scheduler side of the checkpoint stream: one thread, the test machine. */
static CpuState *g_visit_cpu;
void sched_for_each_thread_cpu(SrSchedThreadVisit visit, void *ctx) {
    if (g_visit_cpu) visit(ctx, 1u, 2, 0x20, 0u, g_visit_cpu);
}
uint64_t sched_vtime_us(void) { return 1234u; }
int sched_clock_deterministic(void) { return 1; }

/* ---- harness ---------------------------------------------------------------------- */

#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
#endif

static int g_failed = 0;
static unsigned g_checks = 0;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { fprintf(stderr, "FAIL L%d: ", __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); g_failed = 1; } \
} while (0)

#define CODE_BASE   0x08810000u
#define CODE_END    0x08811000u
#define DATA_BASE   0x08900000u
#define DATA_SIZE   0x00004000u
#define STATE_ADDR  DATA_BASE            /* h, x, frame counter */
#define OUT_ADDR    (DATA_BASE + 0x100u) /* 16 words per frame */
#define FRAMES      24u                  /* N; the uninterrupted run is 2N */

enum { ZERO = 0, V1 = 3, A0 = 4, A1 = 5, T0 = 8, T1, T2, T3, T4, T5, T6, RA = 31 };

static uint32_t i_type(unsigned op, unsigned rs, unsigned rt, uint16_t imm) {
    return (op << 26) | (rs << 21) | (rt << 16) | imm;
}
static uint32_t r_type(unsigned rs, unsigned rt, unsigned rd, unsigned sa, unsigned funct) {
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct;
}

/* The decoder: a0 = state block, a1 = output base. */
static void install_program(void) {
    const uint32_t prog[] = {
        i_type(0x23, A0, T0, 0),            /*  0 lw    t0, 0(a0)     h            */
        i_type(0x23, A0, T1, 4),            /*  4 lw    t1, 4(a0)     x            */
        i_type(0x23, A0, T2, 8),            /*  8 lw    t2, 8(a0)     frame        */
        i_type(0x0F, 0, T3, 0x41C6),        /* 12 lui   t3, 0x41c6                 */
        i_type(0x0D, T3, T3, 0x4E6D),       /* 16 ori   t3, t3, 0x4e6d  (LCG mult) */
        i_type(0x09, ZERO, T4, 16),         /* 20 addiu t4, zero, 16               */
        r_type(0, T2, T5, 6, 0x00),         /* 24 sll   t5, t2, 6                  */
        r_type(T5, A1, T5, 0, 0x21),        /* 28 addu  t5, t5, a1                 */
        r_type(T1, T3, 0, 0, 0x19),         /* 32 multu t1, t3          loop:      */
        r_type(0, 0, T1, 0, 0x12),          /* 36 mflo  t1                         */
        i_type(0x09, T1, T1, 12345),        /* 40 addiu t1, t1, 12345              */
        r_type(0, T0, T6, 1, 0x02),         /* 44 srl   t6, t0, 1                  */
        r_type(T6, T1, T0, 0, 0x21),        /* 48 addu  t0, t6, t1    h = h/2 + x  */
        r_type(V1, T0, V1, 0, 0x26),        /* 52 xor   v1, v1, t0    running sum  */
        i_type(0x2B, T5, T0, 0),            /* 56 sw    t0, 0(t5)                  */
        i_type(0x09, T5, T5, 4),            /* 60 addiu t5, t5, 4                  */
        i_type(0x09, T4, T4, 0xFFFF),       /* 64 addiu t4, t4, -1                 */
        i_type(0x05, T4, ZERO, 0xFFF6),     /* 68 bne   t4, zero, loop             */
        0u,                                 /* 72 nop                              */
        i_type(0x09, T2, T2, 1),            /* 76 addiu t2, t2, 1                  */
        i_type(0x2B, A0, T0, 0),            /* 80 sw    t0, 0(a0)                  */
        i_type(0x2B, A0, T1, 4),            /* 84 sw    t1, 4(a0)                  */
        i_type(0x2B, A0, T2, 8),            /* 88 sw    t2, 8(a0)                  */
        0x03E00008u,                        /* 92 jr    ra                         */
        0u,                                 /* 96 nop                              */
    };
    for (uint32_t i = 0; i < sizeof prog / sizeof prog[0]; i++)
        MEM_W32_PC(CODE_BASE + i * 4u, prog[i], CODE_BASE + i * 4u);
}

static uint8_t *data_host(void) { return g_mem + (DATA_BASE - SR_RAM_BASE); }

static void fresh_machine(CpuState *s) {
    memset(s, 0, sizeof *s);
    sr_cpu_clear_flow(s);
    s->r[V1] = 0x55AA55AAu;
    memset(data_host(), 0, DATA_SIZE);
    MEM_W32(STATE_ADDR + 0u, 1u);
    MEM_W32(STATE_ADDR + 4u, 0x01234567u);
    MEM_W32(STATE_ADDR + 8u, 0u);
}

typedef struct {
    uint32_t v1, hi, lo, h, x, frame;
} FrameTrace;

static int run_frame(CpuState *s, FrameTrace *trace) {
    s->r[A0] = STATE_ADDR;
    s->r[A1] = OUT_ADDR;
    s->r[RA] = SR_HOST_RETURN_PC;
    s->pc = CODE_BASE;
    int rc = dispatch_call_try(s, CODE_BASE, SR_HOST_RETURN_PC);
    if (rc != SR_GUEST_INTERP_CALL_RETURN) {
        fprintf(stderr, "frame did not return to the host: rc=%d (%s)\n", rc,
                sr_guest_interp_result_name((SrGuestInterpResult)rc));
        return 0;
    }
    trace->v1 = s->r[V1];
    trace->hi = s->hi;
    trace->lo = s->lo;
    trace->h = MEM_R32(STATE_ADDR + 0u);
    trace->x = MEM_R32(STATE_ADDR + 4u);
    trace->frame = MEM_R32(STATE_ADDR + 8u);
    return 1;
}

/* ---- exporters for the round trip -------------------------------------------------- */

typedef struct {
    NkStateThread threads[1];
} CpuCtx;

typedef struct {
    NkStateMemRegion regions[1];
} MemCtx;

static NkStateStatus cpu_save(void *ctx, NkStateBuf *out, char *detail, size_t cap) {
    CpuCtx *c = (CpuCtx *)ctx;
    return nkstate_cpu_save(c->threads, 1u, out, detail, cap);
}
static NkStateStatus cpu_load(void *ctx, const uint8_t *p, size_t len, char *detail, size_t cap) {
    CpuCtx *c = (CpuCtx *)ctx;
    return nkstate_cpu_load(c->threads, 1u, p, len, detail, cap);
}
/* A loader that drops a field: restore must catch it. */
static NkStateStatus cpu_load_lossy(void *ctx, const uint8_t *p, size_t len, char *detail, size_t cap) {
    CpuCtx *c = (CpuCtx *)ctx;
    NkStateStatus st = nkstate_cpu_load(c->threads, 1u, p, len, detail, cap);
    if (st == NKSTATE_OK) c->threads[0].cpu->hi ^= 1u;
    return st;
}
static NkStateStatus mem_save(void *ctx, NkStateBuf *out, char *detail, size_t cap) {
    MemCtx *m = (MemCtx *)ctx;
    return nkstate_mem_save(m->regions, 1u, out, detail, cap);
}
static NkStateStatus mem_load(void *ctx, const uint8_t *p, size_t len, char *detail, size_t cap) {
    MemCtx *m = (MemCtx *)ctx;
    return nkstate_mem_load(m->regions, 1u, p, len, detail, cap);
}
static NkStateStatus ge_busy_save(void *ctx, NkStateBuf *out, char *detail, size_t cap) {
    (void)ctx; (void)out;
    snprintf(detail, cap, "a display list is mid-execution");
    return NKSTATE_NOT_QUIET;
}
static NkStateStatus never_load(void *ctx, const uint8_t *p, size_t len, char *detail, size_t cap) {
    (void)ctx; (void)p; (void)len; (void)detail; (void)cap;
    return NKSTATE_OK;
}

static NkStateIdentity test_identity(void) {
    NkStateIdentity id;
    memset(&id, 0, sizeof id);
    id.kind = NKSTATE_KIND_STATE;
    for (unsigned i = 0; i < 32u; i++) id.executable[i] = (uint8_t)(0xA0u + i);
    memcpy(id.runtime, "4f1d2c3b4a5968778695a4b3c2d1e0f001122334", 40);
    memcpy(id.disc_id, "NKTEST00001", 11);
    id.flags = NKSTATE_FLAG_DETERMINISTIC_CLOCK;
    return id;
}

static const NkStateRequirement k_required[] = {
    {"CPU ", "cpu"},
    {"MEM ", "guest memory"},
};

static CpuCtx g_cpu_ctx;
static MemCtx g_mem_ctx;

static void bind_exporters(CpuState *s, NkStateExporter ex[2]) {
    g_cpu_ctx.threads[0].uid = 1u;
    g_cpu_ctx.threads[0].cpu = s;
    g_mem_ctx.regions[0].addr = DATA_BASE;
    g_mem_ctx.regions[0].size = DATA_SIZE;
    g_mem_ctx.regions[0].host = data_host();
    NkStateExporter cpu = {"CPU ", "cpu", 1u, cpu_save, cpu_load, &g_cpu_ctx};
    NkStateExporter mem = {"MEM ", "guest memory", 1u, mem_save, mem_load, &g_mem_ctx};
    ex[0] = cpu;
    ex[1] = mem;
}

static void clobber(CpuState *s) {
    memset(s, 0xA5, sizeof *s);
    memset(data_host(), 0x5A, DATA_SIZE);
}

/* ---- the round trip ---------------------------------------------------------------- */

static void test_round_trip_continues_identically(void) {
    CpuState s;
    FrameTrace want[2u * FRAMES], got[2u * FRAMES];
    uint8_t want_data[DATA_SIZE];
    CpuState want_cpu;
    char detail[256];

    /* Uninterrupted. */
    fresh_machine(&s);
    for (uint32_t i = 0; i < 2u * FRAMES; i++)
        CHECK(run_frame(&s, &want[i]), "uninterrupted frame %u", i);
    memcpy(want_data, data_host(), DATA_SIZE);
    want_cpu = s;
    CHECK(want[2u * FRAMES - 1u].frame == 2u * FRAMES, "the guest counted %u frames",
          want[2u * FRAMES - 1u].frame);

    /* Interrupted: N frames, capture, write, clobber, read, restore, N more. */
    fresh_machine(&s);
    for (uint32_t i = 0; i < FRAMES; i++) CHECK(run_frame(&s, &got[i]), "first-half frame %u", i);

    NkStateExporter ex[2];
    bind_exporters(&s, ex);
    NkStateIdentity self = test_identity();
    static NkCheckpoint at_save, after_load;
    nk_ckpt_begin(&at_save, FRAMES);
    nk_ckpt_add_cpu(&at_save, 1u, &s);
    nk_ckpt_add_chunks(&at_save, "ram", DATA_BASE, data_host(), DATA_SIZE, 0x1000u);
    NkStateBuf image = {0};
    NkStateStatus st = nkstate_capture(&self, k_required, 2u, ex, 2u, &image, detail, sizeof detail);
    CHECK(st == NKSTATE_OK, "capture: %s %s", nkstate_status_name(st), detail);
    const char *path = SR_SELFTEST_BUILD_ROOT "/nkstate_selftest.nkstate";
    st = nkstate_write_file(path, &image, detail, sizeof detail);
    CHECK(st == NKSTATE_OK, "write: %s %s", nkstate_status_name(st), detail);
    nkstate_buf_free(&image);

    /* Control: continuing from a clobbered machine without a restore must differ. */
    {
        CpuState c;
        uint8_t keep[DATA_SIZE];
        memcpy(keep, data_host(), DATA_SIZE);
        c = s;
        clobber(&c);
        memset(data_host(), 0x5A, DATA_SIZE);
        MEM_W32(STATE_ADDR + 8u, FRAMES); /* keep the output pointer in the window */
        FrameTrace t;
        int ran = run_frame(&c, &t);
        CHECK(!ran || t.v1 != want[FRAMES].v1 || t.h != want[FRAMES].h,
              "a clobbered machine continued like the original, so the round trip proves nothing");
        memcpy(data_host(), keep, DATA_SIZE);
    }

    clobber(&s);
    NkStateBuf read = {0};
    st = nkstate_read_file(path, &read, detail, sizeof detail);
    CHECK(st == NKSTATE_OK, "read: %s %s", nkstate_status_name(st), detail);
    NkStateExpect expect = {&self, 1, ex, 2u};
    st = nkstate_restore(read.data, read.len, &expect, detail, sizeof detail);
    CHECK(st == NKSTATE_OK, "restore: %s %s", nkstate_status_name(st), detail);
    /* The restored machine digests exactly as it did when it was saved. */
    nk_ckpt_begin(&after_load, FRAMES);
    nk_ckpt_add_cpu(&after_load, 1u, &s);
    nk_ckpt_add_chunks(&after_load, "ram", DATA_BASE, data_host(), DATA_SIZE, 0x1000u);
    {
        NkCkptMismatch m;
        CHECK(nk_ckpt_compare(&at_save, &after_load, &m) == NK_CKPT_SAME,
              "the restored machine's checkpoint differs at '%s'", m.name);
    }

    for (uint32_t i = FRAMES; i < 2u * FRAMES; i++) CHECK(run_frame(&s, &got[i]), "second-half frame %u", i);
    for (uint32_t i = 0; i < 2u * FRAMES; i++)
        CHECK(memcmp(&got[i], &want[i], sizeof got[i]) == 0,
              "frame %u differs: v1 0x%08x/0x%08x h 0x%08x/0x%08x x 0x%08x/0x%08x hi 0x%08x/0x%08x", i,
              got[i].v1, want[i].v1, got[i].h, want[i].h, got[i].x, want[i].x, got[i].hi, want[i].hi);
    CHECK(memcmp(data_host(), want_data, DATA_SIZE) == 0, "the data window differs after the round trip");
    CHECK(memcmp(&s, &want_cpu, sizeof s) == 0, "the final CpuState differs after the round trip");

    /* A lossy loader is caught by the save-back check before the guest would run. */
    {
        NkStateExporter lossy[2];
        bind_exporters(&s, lossy);
        lossy[0].load = cpu_load_lossy;
        NkStateExpect e2 = {&self, 1, lossy, 2u};
        st = nkstate_restore(read.data, read.len, &e2, detail, sizeof detail);
        CHECK(st == NKSTATE_RESTORE_VERIFY, "a lossy CPU loader gave %s", nkstate_status_name(st));
        CHECK(strstr(detail, "CPU ") != NULL, "restore-verify does not name the segment: %s", detail);
    }

    /* A refusal before the first load leaves the target untouched. */
    {
        uint8_t before_data[DATA_SIZE];
        CpuState before_cpu;
        clobber(&s);
        memcpy(before_data, data_host(), DATA_SIZE);
        before_cpu = s;
        read.data[read.len - 40u] ^= 0x01u; /* inside the last payload */
        st = nkstate_restore(read.data, read.len, &expect, detail, sizeof detail);
        CHECK(st == NKSTATE_SEGMENT_HASH, "a tampered payload gave %s", nkstate_status_name(st));
        CHECK(memcmp(data_host(), before_data, DATA_SIZE) == 0 && memcmp(&s, &before_cpu, sizeof s) == 0,
              "a refused restore changed the target");
    }
    nkstate_buf_free(&read);
}

/* ---- refusals ---------------------------------------------------------------------- */

static uint8_t g_seg_a[] = "segment-a";
static uint8_t g_seg_b[] = "segment-b-longer";

static NkStateStatus buf_save(void *ctx, NkStateBuf *out, char *detail, size_t cap) {
    const uint8_t *p = (const uint8_t *)ctx;
    (void)detail; (void)cap;
    return nkstate_buf_put(out, p, strlen((const char *)p)) ? NKSTATE_OK : NKSTATE_NO_MEMORY;
}

static NkStateBuf make_image(const NkStateIdentity *id, NkStateExporter *ex, size_t n) {
    NkStateBuf image = {0};
    char detail[256];
    NkStateStatus st = nkstate_capture(id, NULL, 0u, ex, n, &image, detail, sizeof detail);
    CHECK(st == NKSTATE_OK, "make_image: %s %s", nkstate_status_name(st), detail);
    return image;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Recompute the file hash after editing the header or table, so a test reaches the check
 * it means to reach. */
static void rehash(NkStateBuf *b) {
    sr_vfpu_sha256(b->data, b->len - 32u, b->data + b->len - 32u);
}

static void expect_refusal(const char *what, const uint8_t *data, size_t len, const NkStateExpect *e,
                           NkStateStatus want) {
    NkStateFile f;
    char detail[256] = {0};
    NkStateStatus st = nkstate_parse(data, len, e, &f, detail, sizeof detail);
    CHECK(st == want, "%s: got %s (%s), want %s", what, nkstate_status_name(st), detail, nkstate_status_name(want));
    CHECK(st == NKSTATE_OK || detail[0] != '\0', "%s: refusal %s has no detail", what, nkstate_status_name(st));
    if (st == NKSTATE_OK) nkstate_file_free(&f);
}

#define OFF_FORMAT   8u
#define OFF_KIND     12u
#define OFF_ABI      16u
#define OFF_CPUSIZE  20u
#define OFF_EXE      24u
#define OFF_RUNTIME  56u
#define OFF_DISC     120u
#define OFF_FLAGS    136u
#define OFF_COUNT    140u
#define OFF_TABLE    144u

static void test_refusals(void) {
    NkStateIdentity self = test_identity();
    NkStateExporter ex[2] = {
        {"SEGA", "a", 1u, buf_save, never_load, g_seg_a},
        {"SEGB", "b", 2u, buf_save, never_load, g_seg_b},
    };
    NkStateExpect expect = {&self, 1, ex, 2u};
    NkStateBuf good = make_image(&self, ex, 2u);
    NkStateBuf b = {0};
    char detail[256];
    NkStateFile f;

    CHECK(NKSTATE_HEADER_SIZE == OFF_TABLE, "header size %u", NKSTATE_HEADER_SIZE);
    {
        NkStateStatus st = nkstate_parse(good.data, good.len, &expect, &f, detail, sizeof detail);
        CHECK(st == NKSTATE_OK, "the good image was refused: %s %s", nkstate_status_name(st), detail);
        if (st == NKSTATE_OK) {
            CHECK(f.segment_count == 2u && memcmp(f.segments[1].payload, g_seg_b, f.segments[1].length) == 0,
                  "parsed segments differ");
            nkstate_file_free(&f);
        }
    }

#define MUTATE(label, edit, want) do { \
        nkstate_buf_put(&b, good.data, good.len); \
        { uint8_t *d = b.data; (void)d; edit; } \
        expect_refusal(label, b.data, b.len, &expect, want); \
        b.len = 0u; \
    } while (0)

    MUTATE("bad magic", d[0] = 'X', NKSTATE_BAD_MAGIC);
    MUTATE("format version", put32(d + OFF_FORMAT, 2u), NKSTATE_FORMAT_VERSION_MISMATCH);
    MUTATE("kind", put32(d + OFF_KIND, NKSTATE_KIND_RECORDING), NKSTATE_KIND_MISMATCH);
    MUTATE("abi version", put32(d + OFF_ABI, SR_CPUSTATE_ABI_VERSION + 1u), NKSTATE_ABI_VERSION);
    MUTATE("cpustate size", put32(d + OFF_CPUSIZE, (uint32_t)sizeof(CpuState) + 4u), NKSTATE_CPUSTATE_SIZE);
    MUTATE("executable", d[OFF_EXE + 31u] ^= 1u, NKSTATE_EXECUTABLE_IDENTITY);
    MUTATE("runtime", d[OFF_RUNTIME] = 'f', NKSTATE_RUNTIME_IDENTITY);
    MUTATE("disc id", d[OFF_DISC + 10u] = '2', NKSTATE_DISC_ID);
    MUTATE("unknown flag", put32(d + OFF_FLAGS, 0x3u), NKSTATE_FLAGS);
    MUTATE("not deterministic", put32(d + OFF_FLAGS, 0u), NKSTATE_CLOCK_NOT_DETERMINISTIC);
    MUTATE("too many segments", put32(d + OFF_COUNT, NKSTATE_MAX_SEGMENTS + 1u), NKSTATE_TOO_MANY_SEGMENTS);
    MUTATE("bad tag", d[OFF_TABLE] = 0x01u, NKSTATE_BAD_TAG);
    MUTATE("duplicate", memcpy(d + OFF_TABLE + NKSTATE_TABLE_ENTRY_SIZE, "SEGA", 4), NKSTATE_DUPLICATE_SEGMENT);
    MUTATE("unknown segment", memcpy(d + OFF_TABLE, "SEGZ", 4), NKSTATE_UNKNOWN_SEGMENT);
    MUTATE("segment version", put32(d + OFF_TABLE + 4u, 9u), NKSTATE_SEGMENT_VERSION);
    MUTATE("segment hash", d[OFF_TABLE + 2u * NKSTATE_TABLE_ENTRY_SIZE] ^= 1u, NKSTATE_SEGMENT_HASH);
    MUTATE("segment hash after rehash", (d[OFF_TABLE + 2u * NKSTATE_TABLE_ENTRY_SIZE] ^= 1u, rehash(&b)),
           NKSTATE_SEGMENT_HASH);
    MUTATE("file hash", d[b.len - 1u] ^= 1u, NKSTATE_FILE_HASH);
    MUTATE("hash in the table", d[OFF_TABLE + 16u] ^= 1u, NKSTATE_SEGMENT_HASH);
    {
        /* Flags bit 0 is not an identity check when the reader does not require the
         * deterministic clock, so only the file hash catches the edit. */
        NkStateExpect lax = expect;
        lax.require_deterministic_clock = 0;
        nkstate_buf_put(&b, good.data, good.len);
        put32(b.data + OFF_FLAGS, 0u);
        expect_refusal("flags edited, file hash", b.data, b.len, &lax, NKSTATE_FILE_HASH);
        b.len = 0u;
    }
    {
        nkstate_buf_put(&b, good.data, good.len);
        nkstate_buf_put(&b, "x", 1u);
        expect_refusal("trailing bytes", b.data, b.len, &expect, NKSTATE_TRAILING_BYTES);
        b.len = 0u;
    }
    {
        /* A segment length past the end. */
        nkstate_buf_put(&b, good.data, good.len);
        put32(b.data + OFF_TABLE + 8u, 0x7FFFFFFFu);
        expect_refusal("segment length", b.data, b.len, &expect, NKSTATE_TRUNCATED);
        put32(b.data + OFF_TABLE + 8u, 0u);
        put32(b.data + OFF_TABLE + 12u, 0x80000000u); /* high word of the u64 length */
        expect_refusal("segment length high word", b.data, b.len, &expect, NKSTATE_TRUNCATED);
        b.len = 0u;
    }
    /* Every prefix of a good file is refused, never accepted and never read past. */
    for (size_t n = 0; n < good.len; n++) {
        NkStateStatus st = nkstate_parse(good.data, n, &expect, &f, detail, sizeof detail);
        if (st == NKSTATE_OK) nkstate_file_free(&f);
        CHECK(st == NKSTATE_TRUNCATED,
              "prefix of %zu bytes gave %s", n, nkstate_status_name(st));
    }
    /* The reader requires every segment it loads, and refuses one it cannot load. */
    {
        NkStateExporter three[3] = {ex[0], ex[1], {"SEGC", "c", 1u, buf_save, never_load, g_seg_a}};
        NkStateExpect more = {&self, 1, three, 3u};
        expect_refusal("missing segment", good.data, good.len, &more, NKSTATE_MISSING_SEGMENT);
        NkStateExpect fewer = {&self, 1, ex, 1u};
        expect_refusal("segment this build cannot load", good.data, good.len, &fewer, NKSTATE_UNKNOWN_SEGMENT);
    }
    nkstate_buf_free(&b);
    nkstate_buf_free(&good);
}

static void test_capture_refusals(void) {
    NkStateIdentity self = test_identity();
    NkStateBuf out = {0};
    char detail[256];
    NkStateExporter ex[2] = {
        {"CPU ", "cpu", 1u, buf_save, never_load, g_seg_a},
        {"MEM ", "guest memory", 1u, buf_save, never_load, g_seg_b},
    };
    const NkStateRequirement with_ge[] = {{"CPU ", "cpu"}, {"MEM ", "guest memory"}, {"GE  ", "ge"}};
    NkStateStatus st = nkstate_capture(&self, with_ge, 3u, ex, 2u, &out, detail, sizeof detail);
    CHECK(st == NKSTATE_MISSING_EXPORTER, "a required subsystem with no exporter gave %s", nkstate_status_name(st));
    CHECK(strstr(detail, "'ge'") != NULL, "missing-exporter does not name the subsystem: %s", detail);
    CHECK(out.data == NULL && out.len == 0u, "a refused capture produced bytes");

    NkStateExporter half[2] = {ex[0], ex[1]};
    half[1].load = NULL;
    st = nkstate_capture(&self, k_required, 2u, half, 2u, &out, detail, sizeof detail);
    CHECK(st == NKSTATE_MISSING_EXPORTER && strstr(detail, "load") != NULL,
          "an exporter with no load gave %s (%s)", nkstate_status_name(st), detail);

    NkStateExporter busy[3] = {ex[0], ex[1], {"GE  ", "ge", 1u, ge_busy_save, never_load, NULL}};
    st = nkstate_capture(&self, with_ge, 3u, busy, 3u, &out, detail, sizeof detail);
    CHECK(st == NKSTATE_NOT_QUIET && strstr(detail, "mid-execution") != NULL && strstr(detail, "'ge'") != NULL,
          "a busy subsystem gave %s (%s)", nkstate_status_name(st), detail);
    CHECK(out.data == NULL, "a not-quiet capture produced bytes");

    NkStateIdentity anon = self;
    memset(anon.runtime, 0, sizeof anon.runtime);
    st = nkstate_capture(&anon, k_required, 2u, ex, 2u, &out, detail, sizeof detail);
    CHECK(st == NKSTATE_NO_RUNTIME_IDENTITY, "a build with no runtime identity gave %s", nkstate_status_name(st));

    NkStateExporter dup[2] = {ex[0], ex[0]};
    st = nkstate_capture(&self, NULL, 0u, dup, 2u, &out, detail, sizeof detail);
    CHECK(st == NKSTATE_DUPLICATE_SEGMENT, "two exporters with one tag gave %s", nkstate_status_name(st));
}

static void test_segment_payload_refusals(void) {
    CpuState a, b2;
    memset(&a, 0x11, sizeof a);
    memset(&b2, 0, sizeof b2);
    NkStateThread t1[1] = {{1u, &a}}, t2[1] = {{2u, &b2}};
    NkStateBuf out = {0};
    char detail[256];
    CHECK(nkstate_cpu_save(t1, 1u, &out, detail, sizeof detail) == NKSTATE_OK, "cpu save");
    CHECK(out.len == 4u + 4u + sizeof(CpuState), "CPU segment is %zu bytes", out.len);
    CHECK(nkstate_cpu_load(t2, 1u, out.data, out.len, detail, sizeof detail) == NKSTATE_SEGMENT_PAYLOAD,
          "a state for another thread uid was loaded");
    CHECK(nkstate_cpu_load(t1, 1u, out.data, out.len - 1u, detail, sizeof detail) == NKSTATE_TRUNCATED,
          "a short CPU segment was loaded");
    nkstate_buf_free(&out);

    uint8_t src[64], dst[64];
    memset(src, 7, sizeof src);
    memset(dst, 0, sizeof dst);
    NkStateMemRegion r1[1] = {{0x08900000u, 64u, src}}, r2[1] = {{0x08900040u, 64u, dst}};
    CHECK(nkstate_mem_save(r1, 1u, &out, detail, sizeof detail) == NKSTATE_OK, "mem save");
    CHECK(nkstate_mem_load(r2, 1u, out.data, out.len, detail, sizeof detail) == NKSTATE_SEGMENT_PAYLOAD,
          "a region at another address was loaded");
    CHECK(dst[0] == 0u, "a refused region load wrote bytes");
    NkStateMemRegion r3[1] = {{0x08900000u, 64u, dst}};
    CHECK(nkstate_mem_load(r3, 1u, out.data, out.len, detail, sizeof detail) == NKSTATE_OK, "mem load");
    CHECK(memcmp(src, dst, sizeof src) == 0, "mem load copied different bytes");
    nkstate_buf_free(&out);
}

/* One fixed file that tools/test_nkstate.py builds byte for byte too: the same identity,
 * one segment "TEST" version 1 holding "nakagawa-state". */
#define GOLDEN_SHA256 "2be4fb0c0c28c07cc701b8a1ba35283fcaad29bee3fa8b590b9a8ac2f8555a2d"

static void test_golden(void) {
    NkStateIdentity self = test_identity();
    static uint8_t payload[] = "nakagawa-state";
    NkStateExporter ex[1] = {{"TEST", "test", 1u, buf_save, never_load, payload}};
    NkStateBuf image = make_image(&self, ex, 1u);
    uint8_t digest[32];
    char hex[65];
    sr_vfpu_sha256(image.data, image.len, digest);
    for (unsigned i = 0; i < 32u; i++) snprintf(hex + i * 2u, 3, "%02x", digest[i]);
    CHECK(SR_CPUSTATE_ABI_VERSION == 3u && sizeof(CpuState) == 1000u,
          "the golden file assumes CpuState ABI 3 and 1000 bytes; update GOLDEN_SHA256 here "
          "and in tools/test_nkstate.py together");
    CHECK(strcmp(hex, GOLDEN_SHA256) == 0, "golden file digest %s, want %s", hex, GOLDEN_SHA256);
    nkstate_buf_free(&image);
}

/* ---- checkpoints (#2147) ------------------------------------------------------------ */

#define CKPT_CHUNK 0x1000u
#define CKPT_LINE_CAP 4096u

typedef enum { INJECT_NONE, INJECT_RAM, INJECT_REG } Inject;

static void checkpoint_machine(const CpuState *s, uint64_t frame, NkCheckpoint *c) {
    nk_ckpt_begin(c, frame);
    nk_ckpt_add_cpu(c, 1u, s);
    nk_ckpt_add_chunks(c, "ram", DATA_BASE, data_host(), DATA_SIZE, CKPT_CHUNK);
}

/* 2N frames from the fixed start, one checkpoint line after each frame. */
static void run_with_checkpoints(char lines[][CKPT_LINE_CAP], Inject inject, uint32_t at) {
    CpuState s;
    static NkCheckpoint c;
    FrameTrace t;
    fresh_machine(&s);
    for (uint32_t i = 0; i < 2u * FRAMES; i++) {
        CHECK(run_frame(&s, &t), "checkpointed frame %u", i);
        if (i == at && inject == INJECT_RAM) MEM_W8(DATA_BASE + 0x2005u, (uint8_t)(MEM_R8(DATA_BASE + 0x2005u) ^ 1u));
        if (i == at && inject == INJECT_REG) s.r[V1] ^= 1u;
        checkpoint_machine(&s, i, &c);
        CHECK(nk_ckpt_format(&c, lines[i], CKPT_LINE_CAP) != 0u, "checkpoint %u did not format", i);
    }
}

/* The first differing checkpoint between two runs, as a replay reports it. */
static int first_mismatch(char a[][CKPT_LINE_CAP], char b[][CKPT_LINE_CAP], NkCkptMismatch *m) {
    static NkCheckpoint ca, cb;
    char detail[128];
    for (uint32_t i = 0; i < 2u * FRAMES; i++) {
        CHECK(nk_ckpt_parse(a[i], &ca, detail, sizeof detail), "parse a[%u]: %s", i, detail);
        CHECK(nk_ckpt_parse(b[i], &cb, detail, sizeof detail), "parse b[%u]: %s", i, detail);
        if (nk_ckpt_compare(&ca, &cb, m) != NK_CKPT_SAME) return (int)i;
    }
    return -1;
}

static void test_checkpoints(void) {
    static char a[2u * FRAMES][CKPT_LINE_CAP], b[2u * FRAMES][CKPT_LINE_CAP];
    NkCkptMismatch m;
    char detail[128];

    run_with_checkpoints(a, INJECT_NONE, 0u);
    run_with_checkpoints(b, INJECT_NONE, 0u);
    CHECK(first_mismatch(a, b, &m) == -1, "two identical runs differ at a checkpoint");
    CHECK(strncmp(a[0], "NKCKPT v=1 frame=0 n=5 cpu.00000001=", 36) == 0, "line shape: %.60s", a[0]);

    /* A memory difference is named by its chunk, at the frame it happened. */
    run_with_checkpoints(b, INJECT_RAM, 7u);
    CHECK(first_mismatch(a, b, &m) == 7 && m.kind == NK_CKPT_DIGEST && strcmp(m.name, "ram.08902000") == 0,
          "a RAM difference at frame 7 was reported as %s '%s' at frame %llu", nk_ckpt_diff_name(m.kind), m.name,
          (unsigned long long)m.frame);

    /* A register difference is named by its thread, and spreads into memory later. */
    run_with_checkpoints(b, INJECT_REG, 11u);
    CHECK(first_mismatch(a, b, &m) == 11 && m.kind == NK_CKPT_DIGEST && strcmp(m.name, "cpu.00000001") == 0,
          "a register difference at frame 11 was reported as %s '%s'", nk_ckpt_diff_name(m.kind), m.name);

    /* Lines round-trip, and a malformed one is refused with a reason. */
    {
        static NkCheckpoint c, back;
        char line[CKPT_LINE_CAP];
        CHECK(nk_ckpt_parse(a[3], &c, detail, sizeof detail), "parse: %s", detail);
        CHECK(nk_ckpt_format(&c, line, sizeof line) != 0u && strcmp(line, a[3]) == 0, "format(parse(x)) != x");
        CHECK(nk_ckpt_parse(line, &back, detail, sizeof detail) && nk_ckpt_compare(&c, &back, NULL) == NK_CKPT_SAME,
              "a parsed line compares differently");
        const char *bad[] = {
            "NKCKPT v=2 frame=0 n=0\n",
            "NKCKPT v=1 frame=0 n=1\n",
            "NKCKPT v=1 frame=0 n=0 extra\n",
            "NKCKPT v=1 frame=0 n=1 a b=00\n",
            "NKCKPT v=1 frame=x n=0\n",
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
            CHECK(!nk_ckpt_parse(bad[i], &c, detail, sizeof detail) && detail[0], "accepted bad line %zu", i);
        CHECK(nk_ckpt_compare(&c, &c, NULL) == NK_CKPT_SAME, "a checkpoint differs from itself");
    }

    /* The line tools/test_nkstate.py parses (SHA-256 of "abc"). */
    {
        static NkCheckpoint c;
        char line[256];
        nk_ckpt_begin(&c, 42u);
        nk_ckpt_add(&c, "sched", "abc", 3u);
        CHECK(nk_ckpt_format(&c, line, sizeof line) != 0u &&
                  strcmp(line, "NKCKPT v=1 frame=42 n=1 "
                               "sched=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n") == 0,
              "the pinned checkpoint line differs: %s", line);
    }

    /* Names: invalid or too long names mark the checkpoint unusable rather than truncating. */
    {
        static NkCheckpoint c;
        char line[64];
        nk_ckpt_begin(&c, 1u);
        CHECK(!nk_ckpt_add(&c, "has space", "x", 1u) && c.overflow, "a name with a space was accepted");
        CHECK(nk_ckpt_format(&c, line, sizeof line) == 0u, "an overflowed checkpoint was formatted");
        nk_ckpt_begin(&c, 1u);
        CHECK(!nk_ckpt_add(&c, "abcdefghijklmnopqrstuvwx", "x", 1u), "a 24-character name was accepted");
        nk_ckpt_begin(&c, 1u);
        CHECK(nk_ckpt_add(&c, "abcdefghijklmnopqrstuvw", "x", 1u), "a 23-character name was refused");
        static NkCheckpoint d;
        nk_ckpt_begin(&d, 1u);
        CHECK(nk_ckpt_compare(&c, &d, &m) == NK_CKPT_NAMES && strcmp(m.name, "abcdefghijklmnopqrstuvw") == 0,
              "a missing digest was not reported by name");
        nk_ckpt_begin(&d, 2u);
        CHECK(nk_ckpt_compare(&c, &d, &m) == NK_CKPT_FRAME, "different frames were not reported");
    }
}

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

/* The runtime stream: SR_REPLAY_CHECKPOINTS through the VBLANK entry point hle_display.c calls. */
static void test_checkpoint_stream(void) {
    const char *path = SR_SELFTEST_BUILD_ROOT "/nkstate_selftest_checkpoints.txt";
    static const uint32_t vcounts[] = {0u, 1u, 3u, 4u, 5u, 8u}; /* windows of 2: 0 0 1 2 2 4 */
    static const uint32_t want_frames[] = {0u, 3u, 4u, 8u};
    static char text[1u << 16];
    static NkCheckpoint c;
    CpuState s;
    FrameTrace t;
    char detail[128];

    set_env("SR_REPLAY_CHECKPOINTS", path);
    set_env("SR_REPLAY_CHECKPOINT_EVERY", "2");
    fresh_machine(&s);
    g_visit_cpu = &s;
    for (size_t i = 0; i < sizeof vcounts / sizeof vcounts[0]; i++) {
        CHECK(run_frame(&s, &t), "stream frame %zu", i);
        sr_replay_checkpoint_vblank(vcounts[i]);
    }
    g_visit_cpu = NULL;

    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL, "the checkpoint stream %s was not written", path);
    if (!fp) return;
    size_t got = fread(text, 1, sizeof text - 1u, fp);
    fclose(fp);
    text[got] = '\0';
    CHECK(strncmp(text, "NKCKPT_RUN v=1 every=2 clock=deterministic\n", 43) == 0, "stream header: %.60s", text);

    char *line = strchr(text, '\n');
    size_t n = 0;
    while (line && line[1]) {
        char *next = strchr(line + 1, '\n');
        if (next) *next = '\0';
        CHECK(nk_ckpt_parse(line + 1, &c, detail, sizeof detail), "stream line %zu: %s", n, detail);
        if (n < sizeof want_frames / sizeof want_frames[0])
            CHECK(c.frame == want_frames[n], "stream checkpoint %zu is frame %llu, want %u", n,
                  (unsigned long long)c.frame, want_frames[n]);
        /* cpu, sched, 2 VRAM chunks and 64 RAM chunks of 1 MiB. */
        CHECK(c.count == 68u && strcmp(c.digests[0].name, "cpu.00000001") == 0 &&
                  strcmp(c.digests[1].name, "sched") == 0 && strcmp(c.digests[2].name, "vram.04000000") == 0 &&
                  strcmp(c.digests[4].name, "ram.08000000") == 0 && strcmp(c.digests[67].name, "ram.0bf00000") == 0,
              "stream checkpoint %zu has %u digests starting '%s'", n, c.count, c.digests[0].name);
        n++;
        line = next;
    }
    CHECK(n == sizeof want_frames / sizeof want_frames[0], "the stream holds %zu checkpoints, want %zu", n,
          sizeof want_frames / sizeof want_frames[0]);
}

int main(void) {
    sr_mem_init();
    sr_exec_span_reset();
    if (!sr_exec_span_register(CODE_BASE, CODE_END)) {
        fprintf(stderr, "FAIL: code span rejected\n");
        return 1;
    }
    install_program();
    test_round_trip_continues_identically();
    test_refusals();
    test_capture_refusals();
    test_segment_payload_refusals();
    test_golden();
    test_checkpoints();
    test_checkpoint_stream();
    if (g_failed) {
        fprintf(stderr, "NKSTATE SELFTEST FAILED\n");
        return 1;
    }
    printf("ALL NKSTATE TESTS PASSED (%u checks)\n", g_checks);
    return 0;
}
