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
 *
 * Recording and replay (SR_RECORD / SR_REPLAY) run the same decoder in fresh child
 * processes, with on-screen-keyboard requests answered by a fake person: a replay must end in
 * the recorded state without asking the person, and every way it can go wrong (a changed
 * word, an extra poll, an early stop, an edited or truncated file, the host clock) must end
 * in its own named verdict.
 */

#include "recomp.c"   /* arena, dispatch table, dispatch_call_try */
#include "cpu_lle.c"  /* flow helpers the linked interpreter needs */

#include "nkstate.h"
#include "osk_text_entry.h" /* SR_OSK_TEXT_PENDING */
#include "replay_checkpoint.h"
#include "vfpu_tables.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h> /* CreateProcessW: the record/replay children */
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <wchar.h>

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
static int g_child_host_clock; /* the record/replay child's host-clock case */
int sched_clock_deterministic(void) { return !g_child_host_clock; }

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

/* ---- recording and replay in fresh processes (SR_RECORD / SR_REPLAY) ------------------ */

/* The child is this binary run with --rr-child[:<option>,...]. It runs the decoder for 24
 * frames (frames=<n>) and, between frames, asks the on-screen keyboard three times:
 *   request 0 from frame 2: the person answers HELLO (text=<word>) after 3 polls;
 *   request 1 from frame 9: the person cancels after 1 poll;
 *   request 2 at frames 14-16: nobody answers, and the guest abandons it at frame 17
 *   (extra: it polls once more at frame 17 and abandons at 18).
 * Each answer is folded into guest memory, so it decides the rest of the run. Test-only
 * options: perturb=<frame> writes one guest word after that frame; hostclock reports the host
 * clock; hardexit=<frame> stops with _Exit after that frame's VBLANK.
 * At a normal end it prints CHILD_FINAL with the machine's state and the live keyboard calls. */

#define RR_SCRATCH (DATA_BASE + 0x3000u)
#define RR_PERTURB (DATA_BASE + 0x2000u)

static unsigned g_live_polls, g_live_abandons, g_live_request, g_live_request_polls;

typedef struct {
    unsigned frames;
    int perturb, hard_exit, osk_extra;
    char text[16];
} ChildOptions;

static ChildOptions g_child = {24u, -1, -1, 0, "HELLO"};

/* "--rr-child" alone, or followed by ":" and comma-separated options. Returns 0 on a bad one. */
static int parse_child_options(const char *arg) {
    static const char prefix[] = "--rr-child";
    if (strncmp(arg, prefix, sizeof prefix - 1u) != 0) return 0;
    arg += sizeof prefix - 1u;
    if (!*arg) return 1;
    if (*arg++ != ':') return 0;
    while (*arg) {
        char opt[32];
        size_t n = strcspn(arg, ",");
        if (n == 0 || n >= sizeof opt) return 0;
        memcpy(opt, arg, n);
        opt[n] = '\0';
        arg += n + (arg[n] == ',');
        if (strncmp(opt, "frames=", 7) == 0) g_child.frames = (unsigned)atoi(opt + 7);
        else if (strncmp(opt, "perturb=", 8) == 0) g_child.perturb = atoi(opt + 8);
        else if (strncmp(opt, "hardexit=", 9) == 0) g_child.hard_exit = atoi(opt + 9);
        else if (strcmp(opt, "extra") == 0) g_child.osk_extra = 1;
        else if (strcmp(opt, "hostclock") == 0) g_child_host_clock = 1;
        else if (strncmp(opt, "text=", 5) == 0 && strlen(opt + 5) < sizeof g_child.text) strcpy(g_child.text, opt + 5);
        else return 0;
    }
    return 1;
}

static int fake_live(const wchar_t *desc, const wchar_t *initial, wchar_t *out, int cap, uint32_t input_type) {
    (void)desc; (void)initial; (void)input_type;
    g_live_polls++;
    g_live_request_polls++;
    if (g_live_request == 0u && g_live_request_polls == 4u) {
        const char *text = g_child.text;
        int n = 0;
        for (; text[n] && n < cap - 1; n++) out[n] = (wchar_t)(unsigned char)text[n];
        out[n] = L'\0';
        g_live_request++;
        g_live_request_polls = 0;
        return 1;
    }
    if (g_live_request == 1u && g_live_request_polls == 2u) {
        g_live_request++;
        g_live_request_polls = 0;
        return 0;
    }
    return SR_OSK_TEXT_PENDING;
}

static void fake_abandon(void) {
    g_live_abandons++;
    g_live_request++;
    g_live_request_polls = 0;
}

static void fold_answer(int answer, const wchar_t *text) {
    uint32_t x = MEM_R32(STATE_ADDR + 4u);
    x = x * 33u + (uint32_t)(answer + 2);
    for (uint32_t i = 0; answer == 1 && text[i] && i < 32u; i++) {
        MEM_W16(RR_SCRATCH + 2u * i, (uint16_t)text[i]);
        x = x * 31u + (uint32_t)text[i];
    }
    MEM_W32(STATE_ADDR + 4u, x);
}

static int rr_child(void) {
    const unsigned frames = g_child.frames;
    const int perturb = g_child.perturb;
    const int hard_exit = g_child.hard_exit;
    const unsigned abandon_at = g_child.osk_extra ? 18u : 17u;
    unsigned request = 0;
    CpuState s;
    FrameTrace t;
    fresh_machine(&s);
    g_visit_cpu = &s;
    for (unsigned f = 0; f < frames; f++) {
        if (!run_frame(&s, &t)) return 90;
        if ((int)f == perturb) MEM_W32(RR_PERTURB, 0xDEADBEEFu);
        wchar_t out[64] = L"init";
        if ((request == 0u && f >= 2u) || (request == 1u && f >= 9u)) {
            const int r = sr_replay_osk_poll(L"name", L"init", out, 64, 0u, fake_live);
            if (r != SR_OSK_TEXT_PENDING) {
                fold_answer(r, out);
                request++;
            }
        } else if (request == 2u && f >= 14u) {
            if (f == abandon_at) {
                sr_replay_osk_abandon(fake_abandon);
                fold_answer(-1, out);
                request++;
            } else {
                (void)sr_replay_osk_poll(L"name", L"init", out, 64, 0u, fake_live);
            }
        }
        sr_replay_checkpoint_vblank(f);
        if ((int)f == hard_exit) {
            fflush(stderr);
            _Exit(0);
        }
    }
    fprintf(stderr, "CHILD_FINAL v1=%08x h=%08x x=%08x frame=%u live_polls=%u live_abandons=%u\n", s.r[V1],
            MEM_R32(STATE_ADDR), MEM_R32(STATE_ADDR + 4u), MEM_R32(STATE_ADDR + 8u), g_live_polls, g_live_abandons);
    fflush(stderr);
    exit(0);
}

/* Run this binary with one argument, its stdout and stderr in errpath; the exit status, or
 * -1 when it could not be started or waited for. */
static int run_child(const char *mode, const char *errpath) {
#ifdef _WIN32
    wchar_t self[2048], command[2560], werr[1024], wmode[256];
    DWORD n = GetModuleFileNameW(NULL, self, 2048);
    if (n == 0 || n >= 2048) return -1;
    if (MultiByteToWideChar(CP_UTF8, 0, errpath, -1, werr, 1024) <= 0) return -1;
    if (MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 256) <= 0) return -1;
    if (swprintf(command, 2560, L"\"%ls\" %ls", self, wmode) < 0) return -1;
    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE out = CreateFileW(werr, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) return -1;
    STARTUPINFOW startup;
    PROCESS_INFORMATION child;
    ZeroMemory(&startup, sizeof startup);
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = out;
    startup.hStdError = out;
    if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &child)) {
        CloseHandle(out);
        return -1;
    }
    CloseHandle(out);
    DWORD status = 0;
    const int waited = WaitForSingleObject(child.hProcess, INFINITE) == WAIT_OBJECT_0 &&
                       GetExitCodeProcess(child.hProcess, &status);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return waited ? (int)status : -1;
#else
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) _exit(126);
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
        execl("/proc/self/exe", "nkstate_selftest", mode, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
#endif
}

static char *slurp(const char *path, size_t *len_out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t cap = 1u << 16, len = 0;
    char *buf = (char *)malloc(cap + 1u);
    size_t got;
    while (buf && (got = fread(buf + len, 1, cap - len, fp)) > 0) {
        len += got;
        if (len == cap) {
            char *n = (char *)realloc(buf, cap * 2u + 1u);
            if (!n) { free(buf); buf = NULL; break; }
            buf = n;
            cap *= 2u;
        }
    }
    fclose(fp);
    if (buf) buf[len] = '\0';
    if (len_out) *len_out = len;
    return buf;
}

static int spit(const char *path, const char *text, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;
    const int ok = fwrite(text, 1, len, fp) == len;
    return fclose(fp) == 0 && ok;
}

#define RR_REC   SR_SELFTEST_BUILD_ROOT "/nkstate_rr_record.txt"
#define RR_CUT   SR_SELFTEST_BUILD_ROOT "/nkstate_rr_cut.txt"
#define RR_EDIT  SR_SELFTEST_BUILD_ROOT "/nkstate_rr_edited.txt"
#define RR_OUT   SR_SELFTEST_BUILD_ROOT "/nkstate_rr_child.log"

static char g_child_args[128]; /* the next child's options, comma-separated */

static void rr_opt(const char *opt) {
    if (g_child_args[0]) strcat(g_child_args, ",");
    strcat(g_child_args, opt);
}

static void rr_env(const char *record, const char *replay) {
    set_env("SR_RECORD", record);
    set_env("SR_REPLAY", replay);
    set_env("SR_REPLAY_CHECKPOINTS", "");
    set_env("SR_REPLAY_CHECKPOINT_EVERY", "4");
    set_env("SR_DISC_ID", "");
    g_child_args[0] = '\0';
}

/* Run the child; returns its log (caller frees) and stores its status. */
static char *rr_run(int *status) {
    char mode[160] = "--rr-child";
    if (g_child_args[0]) {
        strcat(mode, ":");
        strcat(mode, g_child_args);
    }
    *status = run_child(mode, RR_OUT);
    char *log = slurp(RR_OUT, NULL);
    if (!log) return calloc(1, 1);
    /* The child's stderr is a text stream: on Windows its lines end in CR LF. */
    char *w = log;
    for (const char *r = log; *r; r++)
        if (*r != '\r') *w++ = *r;
    *w = '\0';
    return log;
}

static const char *final_line(const char *log, char *out, size_t cap) {
    const char *p = strstr(log, "CHILD_FINAL ");
    out[0] = '\0';
    if (!p) return out;
    size_t n = 0;
    while (p[n] && p[n] != '\n' && p[n] != '\r' && n + 1u < cap) n++;
    memcpy(out, p, n);
    out[n] = '\0';
    return out;
}

/* The state part of CHILD_FINAL, without the live-call counts. */
static void final_state(const char *log, char *out, size_t cap) {
    final_line(log, out, cap);
    char *live = strstr(out, " live_polls=");
    if (live) *live = '\0';
}

static void test_record_replay(void) {
    int st;
    char *log, *rec;
    char want[160], got[160], line[200];
    size_t rec_len = 0;

    /* Record. */
    rr_env(RR_REC, "");
    log = rr_run(&st);
    CHECK(st == 0, "record child exit %d: %.300s", st, log);
    final_state(log, want, sizeof want);
    CHECK(want[0] != '\0', "record child printed no final state: %.300s", log);
    free(log);
    rec = slurp(RR_REC, &rec_len);
    CHECK(rec != NULL, "SR_RECORD wrote no file");
    if (!rec) return;
    CHECK(strncmp(rec, "NKREC v=1 every=4 clock=deterministic exe=", 42) == 0, "record header: %.120s", rec);
    CHECK(strstr(rec, " disc=- abi=") != NULL, "record header disc field: %.200s", rec);
    CHECK(strstr(rec, "\nNKANSW v=1 call=osk request=0 pending=3 result=ok units=00000048000000450000004c0000004c0000004f\n") != NULL,
          "request 0 (HELLO after 3 polls) not recorded");
    CHECK(strstr(rec, "\nNKANSW v=1 call=osk request=1 pending=1 result=cancel units=-\n") != NULL,
          "request 1 (cancel after 1 poll) not recorded");
    CHECK(strstr(rec, "\nNKANSW v=1 call=osk request=2 pending=3 result=open units=-\n") != NULL,
          "request 2 was not written as open at the frame-16 checkpoint");
    CHECK(strstr(rec, "\nNKANSW v=1 call=osk request=2 pending=3 result=abandoned units=-\n") != NULL,
          "request 2's abandon not recorded");
    CHECK(rec_len > 20u && strcmp(rec + rec_len - 20u, "\nNKREC_END frame=23\n") == 0, "record end line: %s",
          rec_len > 40u ? rec + rec_len - 40u : rec);
    {
        unsigned ckpts = 0;
        for (const char *p = rec; (p = strstr(p, "\nNKCKPT v=1 frame=")) != NULL; p++) ckpts++;
        CHECK(ckpts == 6u, "the recording holds %u checkpoints, want 6 (frames 0,4,..,20)", ckpts);
    }

    /* Replay in a fresh process, with a person who would type something else. */
    rr_env("", RR_REC);
    rr_opt("text=WORLD");
    log = rr_run(&st);
    final_state(log, got, sizeof got);
    CHECK(st == 0, "replay child exit %d: %.300s", st, log);
    CHECK(strstr(log, "REPLAY_COMPLETE frame=23 checkpoints=6 answers=3\n") != NULL, "no REPLAY_COMPLETE: %.300s", log);
    CHECK(strstr(log, "REPLAY_NOT_RUN") == NULL && strstr(log, "REPLAY_MISMATCH") == NULL, "replay verdicts: %.300s", log);
    CHECK(strcmp(got, want) == 0, "replay ended in a different state:\n  record %s\n  replay %s", want, got);
    CHECK(strstr(final_line(log, line, sizeof line), " live_polls=0 live_abandons=0") != NULL,
          "the replay asked the live keyboard: %s", line);
    free(log);

    /* Control: the same live run with the other person's text ends elsewhere, so the answer
     * the replay served is what made it match. */
    rr_env("", "");
    rr_opt("text=WORLD");
    log = rr_run(&st);
    final_state(log, got, sizeof got);
    CHECK(st == 0 && got[0] && strcmp(got, want) != 0, "control run (no replay, WORLD) matched the recording: %s", got);
    free(log);

    /* One guest word changed after frame 6: the frame-8 checkpoint names its chunk. */
    rr_env("", RR_REC);
    rr_opt("perturb=6");
    log = rr_run(&st);
    CHECK(st == 1 && strstr(log, "REPLAY_MISMATCH frame=8 index=2 kind=digest digest=ram.08900000\n") != NULL,
          "perturbed replay: exit %d: %.300s", st, log);
    CHECK(strstr(log, "REPLAY_NOT_RUN") == NULL && strstr(log, "CHILD_FINAL") == NULL,
          "a mismatch did not stop the run with one verdict: %.300s", log);
    free(log);

    /* The guest polls a request the recording abandoned. */
    rr_env("", RR_REC);
    rr_opt("extra");
    log = rr_run(&st);
    CHECK(st == 1 && strstr(log, "REPLAY_MISSING_ANSWER call=osk request=2 frame=16 detail=polled-after-abandon\n") != NULL,
          "extra poll: exit %d: %.300s", st, log);
    free(log);

    /* A replay that stops early is not a pass. */
    rr_env("", RR_REC);
    rr_opt("frames=10");
    log = rr_run(&st);
    CHECK(strstr(log, "REPLAY_NOT_RUN reached=9 end=23 checkpoints=3/6\n") != NULL && strstr(log, "REPLAY_COMPLETE") == NULL,
          "short replay: exit %d: %.300s", st, log);
    free(log);

    /* Refusals, each before the guest runs. */
    {
        static const struct {
            const char *what, *find, *replace, *want;
        } edits[] = {
            {"another executable", " exe=", " exe=0", "REPLAY_REFUSED reason=executable-identity line=1\n"},
            {"another disc", " disc=- ", " disc=ULUS10000 ", "REPLAY_REFUSED reason=disc-id line=1\n"},
            {"another CpuState ABI", " abi=", " abi=9", "REPLAY_REFUSED reason=abi-version line=1\n"},
            {"a host-clock recording", "clock=deterministic", "clock=host", "REPLAY_REFUSED reason=clock-not-deterministic line=1\n"},
            {"an unknown line", "\nNKREC_END", "\nNKWHAT x=1\nNKREC_END", "REPLAY_REFUSED reason=unknown-line line="},
            {"a request out of order", "request=1 pending=1", "request=4 pending=1", "REPLAY_REFUSED reason=answer-order line="},
            {"a bad answer unit", "units=00000048", "units=0000004G", "REPLAY_REFUSED reason=malformed-answer line="},
            {"a not-a-recording file", "NKREC v=1 ", "NKREX v=1 ", "REPLAY_REFUSED reason=bad-magic line=1\n"},
        };
        for (size_t i = 0; i < sizeof edits / sizeof edits[0]; i++) {
            const char *at = strstr(rec, edits[i].find);
            CHECK(at != NULL, "edit '%s' found no '%s'", edits[i].what, edits[i].find);
            if (!at) continue;
            const size_t head = (size_t)(at - rec), fl = strlen(edits[i].find), rl = strlen(edits[i].replace);
            char *ed = (char *)malloc(rec_len + rl + 1u);
            memcpy(ed, rec, head);
            memcpy(ed + head, edits[i].replace, rl);
            memcpy(ed + head + rl, at + fl, rec_len - head - fl + 1u);
            /* " exe=0" adds a 65th hex digit; keep the field 64 long so the identity check, not
             * the parser, refuses it. */
            if (strcmp(edits[i].replace, " exe=0") == 0) memmove(ed + head + 6u + 63u, ed + head + 6u + 64u, strlen(ed + head + 6u + 64u) + 1u);
            CHECK(spit(RR_EDIT, ed, strlen(ed)), "write %s", RR_EDIT);
            free(ed);
            rr_env("", RR_EDIT);
            log = rr_run(&st);
            CHECK(st == 1 && strstr(log, edits[i].want) != NULL && strstr(log, "CHILD_FINAL") == NULL,
                  "%s: exit %d: %.300s", edits[i].what, st, log);
            free(log);
        }
        /* The last line without its newline: truncated, never read as complete. */
        CHECK(spit(RR_EDIT, rec, rec_len - 1u), "write truncated %s", RR_EDIT);
        rr_env("", RR_EDIT);
        log = rr_run(&st);
        CHECK(st == 1 && strstr(log, "REPLAY_REFUSED reason=truncated") != NULL, "truncated: exit %d: %.300s", st, log);
        free(log);
    }

    /* The deterministic clock is required on both sides. */
    rr_env("", RR_REC);
    rr_opt("hostclock");
    log = rr_run(&st);
    CHECK(st == 1 && strstr(log, "REPLAY_REFUSED reason=clock-not-deterministic line=0") != NULL,
          "replay on the host clock: exit %d: %.300s", st, log);
    free(log);
    rr_env(RR_EDIT, "");
    rr_opt("hostclock");
    log = rr_run(&st);
    CHECK(st == 1 && strstr(log, "SR_RECORD needs SR_DETERMINISTIC_CLOCK=1") != NULL, "record on the host clock: exit %d: %.300s",
          st, log);
    free(log);
    rr_env(RR_EDIT, RR_REC);
    log = rr_run(&st);
    CHECK(st == 1 && strstr(log, "SR_RECORD and SR_REPLAY are both set") != NULL, "record+replay: exit %d: %.300s", st, log);
    free(log);
    free(rec);

    /* A recording cut by _Exit right after the frame-16 checkpoint has no end line: it replays
     * to that checkpoint, request 2's polls before it are answered from the open line, and
     * the abandon at frame 17 is the live run's. */
    rr_env(RR_CUT, "");
    rr_opt("hardexit=16");
    log = rr_run(&st);
    CHECK(st == 0, "cut record exit %d: %.300s", st, log);
    free(log);
    rec = slurp(RR_CUT, &rec_len);
    CHECK(rec && !strstr(rec, "NKREC_END") && !strstr(rec, "result=abandoned"),
          "the cut recording has an end line or the later abandon");
    free(rec);
    rr_env("", RR_CUT);
    log = rr_run(&st);
    CHECK(st == 0 && strstr(log, "has no NKREC_END line") != NULL &&
              strstr(log, "REPLAY_COMPLETE frame=16 checkpoints=5 answers=3\n") != NULL,
          "replay of the cut recording: exit %d: %.300s", st, log);
    CHECK(strstr(final_line(log, line, sizeof line), " live_polls=0 live_abandons=1") != NULL,
          "after the cut recording the abandon should be live: %s", line);
    free(log);
    rr_env("", "");
}

int main(int argc, char **argv) {
    sr_mem_init();
    sr_exec_span_reset();
    if (!sr_exec_span_register(CODE_BASE, CODE_END)) {
        fprintf(stderr, "FAIL: code span rejected\n");
        return 1;
    }
    install_program();
    if (argc == 2 && strncmp(argv[1], "--rr-child", 10) == 0) {
        if (!parse_child_options(argv[1])) {
            fprintf(stderr, "nkstate_selftest: bad child option '%s'\n", argv[1]);
            return 2;
        }
        return rr_child();
    }
    if (argc != 1) {
        fprintf(stderr, "usage: nkstate_selftest\n");
        return 2;
    }
    test_round_trip_continues_identically();
    test_refusals();
    test_capture_refusals();
    test_segment_payload_refusals();
    test_golden();
    test_checkpoints();
    test_checkpoint_stream();
    test_record_replay();
    if (g_failed) {
        fprintf(stderr, "NKSTATE SELFTEST FAILED\n");
        return 1;
    }
    printf("ALL NKSTATE TESTS PASSED (%u checks)\n", g_checks);
    return 0;
}
