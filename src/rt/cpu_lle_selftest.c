// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// LLE Phase 1 (PR 2) CPU selftest: COP0 access, mode checks, exception entry,
// and eret. Standalone host executable, no game inputs required:
//
//   mingw32-make CC=gcc cpu-lle-selftest
//
// The harness #includes recomp.c and cpu_lle.c for direct access to the same
// helpers the generated code and the interpreter call, and links the real
// guest_interp.c so the interpreter lane is exercised, not modeled. The stub
// block below mirrors src/rt/heap_selftest.c: symbols recomp.c references
// that live in scheduler/HLE translation units we deliberately do not link.
//
// Failing-before evidence (PR 2, spec section 11): on the base tree there is
// no cpu_lle.h/.c, generated syscalls use sr_raw_syscall, generated breaks
// use sr_break, and the interpreter answers UNSUPPORTED for syscall/break
// with no eret path, so every assertion here fails to build or fails closed.

#include "recomp.c"   /* arena, dispatch table, MEM_* backing */
#include "cpu_lle.c"  /* helpers under test (white-box, same TU) */

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

static int g_failed = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL L%d: ", __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); g_failed = 1; } \
} while (0)

#define CAUSE_EXCCODE(cause) (((cause) >> 2) & 0x1Fu)
#define CAUSE_CE(cause) (((cause) >> 28) & 3u)

#define TEST_BASE 0x08810000u
#define TEST_SPAN_END 0x08811000u
#define TEST_VECTOR 0x80000180u
#define TEST_VSPAN_END 0x80001000u

/* Measured pre-exception Status for campaign psp-hw-20260917 (PSP-A1-01,
 * PSP-A2-01, PSP-A3-01): KSU user, IE 1, IM 0x86, EXL 0 before entry. The
 * model preserves every pre-exception bit and sets only EXL, so the measured
 * frame 0x00088613 = 0x00088611 | SR_STATUS_EXL is reproduced exactly. */
#define TEST_PRE_STATUS 0x00088611u

static void fresh_state(CpuState *s) {
    memset(s, 0, sizeof *s);
    sr_cpu_lle_reset_config();
    sr_cpu_clear_flow(s);
}

static uint32_t enc_mfc0(unsigned rt, unsigned rd) {
    return (0x10u << 26) | (rt << 16) | (rd << 11);
}

static uint32_t enc_mtc0(unsigned rt, unsigned rd) {
    return (0x10u << 26) | (4u << 21) | (rt << 16) | (rd << 11);
}

/* I-type memory encodings for the address-error tests below. */
static uint32_t enc_mem(unsigned primary, unsigned base, unsigned rt, uint16_t offset) {
    return (primary << 26) | (base << 21) | (rt << 16) | (uint32_t)offset;
}
static uint32_t enc_lw(unsigned base, unsigned rt, uint16_t offset) {
    return enc_mem(0x23u, base, rt, offset);
}
static uint32_t enc_sw(unsigned base, unsigned rt, uint16_t offset) {
    return enc_mem(0x2Bu, base, rt, offset);
}

static void setup_spans(void) {
    sr_exec_span_reset();
    CHECK(sr_exec_span_register(TEST_BASE, TEST_SPAN_END), "test span rejected");
    CHECK(sr_exec_span_register(TEST_VECTOR, TEST_VSPAN_END), "vector span rejected");
}

/* Exception entry sets EPC/Cause/EXL and transfers to the vector. */
static void test_exception_entry_plain(void) {
    CpuState s;
    int rc;
    fresh_state(&s);
    rc = sr_cpu_raise_exception(&s, SR_EXC_SYS, 0x08810000u, 0x08810000u, 0u, 0u, 0u);
    CHECK(rc < 0, "raise must report a transfer");
    CHECK(s.cop0[SR_CP0_EPC] == 0x08810000u, "EPC=0x%08x, want fault pc", s.cop0[SR_CP0_EPC]);
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_SYS, "ExcCode=%u, want SYS",
          CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]));
    CHECK((s.cop0[SR_CP0_CAUSE] & SR_CAUSE_BD) == 0u, "BD must be clear outside a delay slot");
    CHECK((s.cop0[SR_CP0_STATUS] & SR_STATUS_EXL) != 0u, "EXL must be set");
    CHECK(s.flow_kind == SR_FLOW_EXCEPTION, "flow=%u, want EXCEPTION", s.flow_kind);
    CHECK(s.flow_target == TEST_VECTOR, "target=0x%08x, want vector", s.flow_target);
    CHECK(s.pc == TEST_VECTOR, "pc=0x%08x, want vector", s.pc);
}

/* A delay-slot fault names the branch in EPC and sets Cause.BD. */
static void test_exception_delay_slot(void) {
    CpuState s;
    int rc;
    fresh_state(&s);
    rc = sr_cpu_raise_exception(&s, SR_EXC_SYS, 0x08810004u, 0x08810000u, 0u, 1u, 0u);
    CHECK(rc < 0, "delay raise must report a transfer");
    CHECK(s.cop0[SR_CP0_EPC] == 0x08810000u, "EPC=0x%08x, want branch pc", s.cop0[SR_CP0_EPC]);
    CHECK((s.cop0[SR_CP0_CAUSE] & SR_CAUSE_BD) != 0u, "BD must be set for a delay-slot fault");
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_SYS, "ExcCode lost on delay path");
    CHECK(s.flow_kind == SR_FLOW_EXCEPTION && s.pc == TEST_VECTOR, "delay transfer must reach the vector");
}

/* Coprocessor faults carry CE; address faults carry BadVAddr. */
static void test_exception_ce_badvaddr(void) {
    CpuState s;
    fresh_state(&s);
    CHECK(sr_cpu_raise_exception(&s, SR_EXC_CPU, 0x08810000u, 0x08810000u, 0u, 0u, 2u) < 0,
          "CPU raise must transfer");
    CHECK(CAUSE_CE(s.cop0[SR_CP0_CAUSE]) == 2u, "CE=%u, want 2", CAUSE_CE(s.cop0[SR_CP0_CAUSE]));
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_CPU, "ExcCode=%u, want CPU",
          CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]));

    fresh_state(&s);
    CHECK(sr_cpu_raise_exception(&s, SR_EXC_ADEL, 0x08810000u, 0x08810000u, 0xDEAD0001u, 0u, 0u) < 0,
          "ADEL raise must transfer");
    CHECK(s.cop0[SR_CP0_BADVADDR] == 0xDEAD0001u, "BadVAddr=0x%08x", s.cop0[SR_CP0_BADVADDR]);
}

/* Hardware-measured exception-entry cells (campaign psp-hw-20260917,
 * PSP-3000 / 6.61, user-mode PRX probes via PSPLink; fixtures
 * fixtures/psp_oracle/probe_exception_a1.c..a3.c, cells in
 * docs/HARDWARE_ORACLE.md "CPU exception entry"). These checks pin the
 * measured fields only: EPC, Cause.BD/ExcCode for Bp/AdEL, BadVAddr, and the
 * preserved general registers. Cause.CE is a single-console observation for
 * these non-CpU codes (bit 28 read 1 in all three runs), so the CE field is
 * treated as undefined and asserted neither way. Vector base/EBase, BEV/ERL,
 * syscall/CpU exceptions, interrupts, and handler entry/return stay
 * unmeasured and are not pinned here. */

/* PSP-A1-01: a non-delay-slot user-mode `break` reports EPC = the break
 * address (not +4), Cause 0x10000024 (ExcCode 9, BD 0), the measured
 * post-entry Status, and preserved a0-a3/t0-t3. */
static void test_hw_psp_a1_01_break_plain(void) {
    CpuState s;
    int rc;
    static const uint32_t pre[4] = {
        0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u  /* a0-a3 */
    };
    static const uint32_t tmp[4] = {
        0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu, 0xDDDDDDDDu  /* t0-t3 */
    };
    unsigned i;
    fresh_state(&s);
    for (i = 0; i < 4; i++) {
        s.r[4 + i] = pre[i];
        s.r[8 + i] = tmp[i];
    }
    s.cop0[SR_CP0_STATUS] = TEST_PRE_STATUS;
    rc = sr_cpu_raise_exception(&s, SR_EXC_BP, 0x08810000u, 0x08810000u, 0u, 0u, 0u);
    CHECK(rc < 0, "PSP-A1-01 raise must report a transfer");
    CHECK(s.cop0[SR_CP0_EPC] == 0x08810000u, "PSP-A1-01 EPC=0x%08x, want the break pc",
          s.cop0[SR_CP0_EPC]);
    /* The measured Cause cells name BD/ExcCode exactly; bit 28 (CE) read 1 in
     * this single-console campaign but is undefined for non-CpU exceptions, so
     * only BD and ExcCode are asserted. */
    CHECK((s.cop0[SR_CP0_CAUSE] & ~(SR_CAUSE_CE_MASK)) == 0x00000024u,
          "PSP-A1-01 Cause=0x%08x, want BD 0 + ExcCode 9 (CE undefined)",
          s.cop0[SR_CP0_CAUSE]);
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_BP, "PSP-A1-01 ExcCode must be 9");
    CHECK((s.cop0[SR_CP0_CAUSE] & SR_CAUSE_BD) == 0u, "PSP-A1-01 BD must be 0");
    CHECK(s.cop0[SR_CP0_STATUS] == 0x00088613u, "PSP-A1-01 Status=0x%08x, want 0x00088613",
          s.cop0[SR_CP0_STATUS]);
    for (i = 0; i < 4; i++) {
        CHECK(s.r[4 + i] == pre[i], "PSP-A1-01 r[%u] clobbered", 4u + i);
        CHECK(s.r[8 + i] == tmp[i], "PSP-A1-01 r[%u] clobbered", 8u + i);
    }
}

/* PSP-A2-01: the same `break` in the delay slot of an always-taken branch
 * reports EPC = the branch address, Cause 0x90000024 (BD 1, ExcCode 9);
 * neither successor executed, which the single-entry raise models by the
 * transfer itself. */
static void test_hw_psp_a2_01_break_delay_slot(void) {
    CpuState s;
    int rc;
    fresh_state(&s);
    s.cop0[SR_CP0_STATUS] = TEST_PRE_STATUS;
    s.in_delay_slot = 1u;
    s.next_pc = 0x08810004u;  /* PSP-A2-01 branch pc */
    rc = sr_cpu_raise_exception(&s, SR_EXC_BP, 0x08810008u, 0x08810004u, 0u, 1u, 0u);
    CHECK(rc < 0, "PSP-A2-01 raise must report a transfer");
    CHECK(s.cop0[SR_CP0_EPC] == 0x08810004u, "PSP-A2-01 EPC=0x%08x, want the branch pc",
          s.cop0[SR_CP0_EPC]);
    CHECK((s.cop0[SR_CP0_CAUSE] & ~(SR_CAUSE_CE_MASK)) == 0x80000024u,
          "PSP-A2-01 Cause=0x%08x, want BD 1 + ExcCode 9 (CE undefined)",
          s.cop0[SR_CP0_CAUSE]);
    CHECK(s.cop0[SR_CP0_STATUS] == 0x00088613u, "PSP-A2-01 Status=0x%08x, want 0x00088613",
          s.cop0[SR_CP0_STATUS]);
}

/* PSP-A3-01: a user-mode `lw $t6, 4($t5)` with t5 = 0x88000010 raises AdEL:
 * EPC = the load address, Cause 0x10000010 (ExcCode 4, BD 0), BadVAddr =
 * the effective address 0x88000014, the same Status, destination t6
 * unchanged, and execution did not continue. */
static void test_hw_psp_a3_01_adel_load(void) {
    CpuState s;
    int rc;
    unsigned i;
    fresh_state(&s);
    for (i = 0; i < 4; i++) {
        s.r[4 + i] = 0x11111111u + i;      /* a0-a3, A3 probe values */
    }
    s.r[13] = 0x88000010u;                 /* t5 */
    s.r[14] = 0x0BADBABEu;                 /* t6: sentinel destination */
    s.cop0[SR_CP0_STATUS] = TEST_PRE_STATUS;
    rc = sr_cpu_raise_exception(&s, SR_EXC_ADEL, 0x08810020u, 0x08810020u,
                                0x88000014u, 0u, 0u);
    CHECK(rc < 0, "PSP-A3-01 raise must report a transfer");
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_ADEL,
          "PSP-A3-01 ExcCode must be 4 (AdEL)");
    CHECK(s.cop0[SR_CP0_EPC] == 0x08810020u, "PSP-A3-01 EPC=0x%08x, want the lw address",
          s.cop0[SR_CP0_EPC]);
    CHECK(s.cop0[SR_CP0_BADVADDR] == 0x88000014u,
          "PSP-A3-01 BadVAddr=0x%08x, want the effective address",
          s.cop0[SR_CP0_BADVADDR]);
    CHECK(s.cop0[SR_CP0_STATUS] == 0x00088613u, "PSP-A3-01 Status=0x%08x, want 0x00088613",
          s.cop0[SR_CP0_STATUS]);
    CHECK(s.r[14] == 0x0BADBABEu, "PSP-A3-01 t6 must be unchanged");
    CHECK(s.r[13] == 0x88000010u, "PSP-A3-01 t5 must be unchanged");
    for (i = 0; i < 4; i++) {
        CHECK(s.r[4 + i] == 0x11111111u + i, "PSP-A3-01 r[%u] clobbered", 4u + i);
    }
    CHECK(s.flow_kind == SR_FLOW_EXCEPTION && s.pc == TEST_VECTOR,
          "PSP-A3-01 must transfer to the vector, not continue");
}

/* PSP-A3-02 / PSP-A3-03: a misaligned user-mode access to a mapped, writable,
 * 16-byte-aligned buffer. The load case raises AdEL (Cause 0x10000010) and the
 * store case AdES (Cause 0x10000014) -- distinct codes, not one shared address
 * error. In both, BadVAddr is the effective address INCLUDING the misaligned low
 * bits, not the aligned base. That last point is the one a plausible-looking
 * implementation gets wrong, so it is asserted directly. */
static void test_hw_psp_a3_02_03_misaligned_codes_and_badvaddr(void) {
    CpuState s;
    /* The measured bases and effective addresses, from the probe headers. */
    const uint32_t load_base = 0x08821C00u, load_ea = 0x08821C02u;
    const uint32_t store_base = 0x08856400u, store_ea = 0x08856402u;

    /* The decision function must classify each case before anything is raised. */
    fresh_state(&s);
    CHECK(sr_cpu_data_access_fault(&s, load_ea, 4u, 0) == (unsigned)SR_EXC_ADEL,
          "PSP-A3-02 a misaligned load must be classified AdEL");
    CHECK(sr_cpu_data_access_fault(&s, store_ea, 4u, 1) == (unsigned)SR_EXC_ADES,
          "PSP-A3-03 a misaligned store must be classified AdES");
    CHECK(sr_cpu_data_access_fault(&s, load_base, 4u, 0) == 0u,
          "an aligned load at the same base must not fault");
    CHECK(sr_cpu_data_access_fault(&s, store_base, 4u, 1) == 0u,
          "an aligned store at the same base must not fault");

    /* PSP-A3-02: lw $t6, 2($t5), t5 = 0x08821C00. */
    fresh_state(&s);
    s.r[13] = load_base;
    s.r[14] = 0x0BADC0DEu;                 /* t6: the probe's sentinel */
    s.cop0[SR_CP0_STATUS] = TEST_PRE_STATUS;
    CHECK(sr_cpu_raise_exception(&s, SR_EXC_ADEL, 0x088043B8u, 0x088043B8u,
                                 load_ea, 0u, 0u) < 0,
          "PSP-A3-02 raise must report a transfer");
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_ADEL,
          "PSP-A3-02 ExcCode must be 4 (AdEL)");
    CHECK(s.cop0[SR_CP0_EPC] == 0x088043B8u,
          "PSP-A3-02 EPC=0x%08x, want the lw address", s.cop0[SR_CP0_EPC]);
    CHECK(s.cop0[SR_CP0_BADVADDR] == load_ea,
          "PSP-A3-02 BadVAddr=0x%08x, want the misaligned effective address 0x%08x",
          s.cop0[SR_CP0_BADVADDR], load_ea);
    CHECK((s.cop0[SR_CP0_BADVADDR] & 3u) != 0u,
          "PSP-A3-02 BadVAddr must keep its low bits, not be aligned down");
    CHECK(s.r[14] == 0x0BADC0DEu, "PSP-A3-02 t6 must be unchanged by a faulting load");
    CHECK(s.flow_kind == SR_FLOW_EXCEPTION && s.pc == TEST_VECTOR,
          "PSP-A3-02 must transfer to the vector, not continue");

    /* PSP-A3-03: sw $t6, 2($t5), t5 = 0x08856400. */
    fresh_state(&s);
    s.r[13] = store_base;
    s.cop0[SR_CP0_STATUS] = TEST_PRE_STATUS;
    CHECK(sr_cpu_raise_exception(&s, SR_EXC_ADES, 0x08838BB8u, 0x08838BB8u,
                                 store_ea, 0u, 0u) < 0,
          "PSP-A3-03 raise must report a transfer");
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_ADES,
          "PSP-A3-03 ExcCode must be 5 (AdES), distinct from the load case");
    CHECK(s.cop0[SR_CP0_EPC] == 0x08838BB8u,
          "PSP-A3-03 EPC=0x%08x, want the sw address", s.cop0[SR_CP0_EPC]);
    CHECK(s.cop0[SR_CP0_BADVADDR] == store_ea,
          "PSP-A3-03 BadVAddr=0x%08x, want the misaligned effective address 0x%08x",
          s.cop0[SR_CP0_BADVADDR], store_ea);
    CHECK(s.cop0[SR_CP0_STATUS] == 0x00088613u,
          "PSP-A3-03 Status=0x%08x, want 0x00088613", s.cop0[SR_CP0_STATUS]);
}

/* BEV selects the bootstrap vector; an unowned vector fails closed. */
static void test_vector_selection(void) {
    CpuState s;
    uint32_t g, i, b;
    fresh_state(&s);
    s.cop0[SR_CP0_STATUS] = SR_STATUS_BEV;
    CHECK(sr_cpu_in_kernel(&s), "BEV kernel status must read as kernel");
    /* Point bootstrap at an owned span: selection is what is under test,
     * while the raw default (outside the flat arena) must fail closed. */
    sr_cpu_lle_set_vectors(TEST_VECTOR, TEST_VECTOR, TEST_VECTOR);
    CHECK(sr_cpu_raise_exception(&s, SR_EXC_SYS, TEST_BASE, TEST_BASE, 0u, 0u, 0u) < 0,
          "BEV raise must transfer");
    sr_cpu_lle_get_vectors(&g, &i, &b);
    CHECK(s.pc == b && s.flow_target == b, "BEV must select bootstrap 0x%08x (got 0x%08x)", b, s.pc);
    (void)g;
    (void)i;

    /* The untouched bootstrap default is outside the flat arena and must
     * fail closed rather than dispatch blind. */
    fresh_state(&s);
    s.cop0[SR_CP0_STATUS] = SR_STATUS_BEV;
    CHECK(sr_cpu_raise_exception(&s, SR_EXC_SYS, TEST_BASE, TEST_BASE, 0u, 0u, 0u) < 0,
          "default-bootstrap raise must unwind");
    CHECK(s.flow_kind == SR_FLOW_FATAL, "default bootstrap must be FATAL, got %u", s.flow_kind);

    fresh_state(&s);
    sr_cpu_lle_set_vectors(0x09000000u, 0x09000000u, 0x09000000u);
    CHECK(sr_cpu_raise_exception(&s, SR_EXC_SYS, TEST_BASE, TEST_BASE, 0u, 0u, 0u) < 0,
          "unowned vector must still unwind");
    CHECK(s.flow_kind == SR_FLOW_FATAL, "unowned vector must be FATAL, got %u", s.flow_kind);
    CHECK(s.flow_target == 0x09000000u, "fatal target must name the bad vector");
}

/* eret returns to EPC and clears EXL; user-mode and bad targets fail closed. */
static void test_eret(void) {
    CpuState s;
    fresh_state(&s);
    s.cop0[SR_CP0_STATUS] = SR_STATUS_EXL;
    s.cop0[SR_CP0_EPC] = TEST_BASE + 0x10u;
    CHECK(sr_cpu_eret(&s, TEST_BASE) < 0, "eret must report a transfer");
    CHECK(s.flow_kind == SR_FLOW_ERET, "flow=%u, want ERET", s.flow_kind);
    CHECK(s.pc == TEST_BASE + 0x10u && s.flow_target == TEST_BASE + 0x10u,
          "eret must target EPC (pc=0x%08x)", s.pc);
    CHECK((s.cop0[SR_CP0_STATUS] & SR_STATUS_EXL) == 0u, "eret must clear EXL");

    fresh_state(&s);
    s.cop0[SR_CP0_STATUS] = SR_STATUS_KSU_USER;
    s.cop0[SR_CP0_EPC] = TEST_BASE + 0x10u;
    CHECK(sr_cpu_eret(&s, TEST_BASE) < 0, "user eret must unwind");
    CHECK(s.flow_kind == SR_FLOW_EXCEPTION, "user eret must trap, got flow %u", s.flow_kind);
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_CPU, "user eret must trap as CPU");
    CHECK(s.cop0[SR_CP0_EPC] == TEST_BASE, "trap EPC must be the eret pc");

    fresh_state(&s);
    s.cop0[SR_CP0_STATUS] = SR_STATUS_EXL;
    s.cop0[SR_CP0_EPC] = TEST_BASE + 0x3u;  /* misaligned */
    CHECK(sr_cpu_eret(&s, TEST_BASE) < 0, "misaligned eret target must unwind");
    CHECK(s.flow_kind == SR_FLOW_FATAL, "misaligned eret target must be FATAL");
}

/* Kernel/user mode follows KSU unless EXL/ERL pin kernel mode. */
static void test_mode_checks(void) {
    CpuState s;
    fresh_state(&s);
    CHECK(sr_cpu_in_kernel(&s), "zero status must be kernel");
    s.cop0[SR_CP0_STATUS] = SR_STATUS_KSU_USER;
    CHECK(!sr_cpu_in_kernel(&s), "KSU=user must read as user");
    s.cop0[SR_CP0_STATUS] = SR_STATUS_KSU_USER | SR_STATUS_EXL;
    CHECK(sr_cpu_in_kernel(&s), "EXL must pin kernel mode");
    s.cop0[SR_CP0_STATUS] = SR_STATUS_KSU_USER | SR_STATUS_ERL;
    CHECK(sr_cpu_in_kernel(&s), "ERL must pin kernel mode");
}

/* MFC0/MTC0 round-trip with Status/Cause masking in kernel mode. */
static void test_cop0_roundtrip(void) {
    CpuState s;
    fresh_state(&s);
    s.r[5] = 0x08810020u;
    CHECK(sr_cp0_mtc0(&s, 5u, SR_CP0_EPC, 0u, TEST_BASE) == 0, "kernel mtc0 EPC must succeed");
    CHECK(s.cop0[SR_CP0_EPC] == 0x08810020u, "EPC not stored");
    s.r[6] = 0u;
    CHECK(sr_cp0_mfc0(&s, 6u, SR_CP0_EPC, 0u, TEST_BASE + 4u) == 0, "kernel mfc0 EPC must succeed");
    CHECK(s.r[6] == 0x08810020u, "EPC round-trip gave 0x%08x", s.r[6]);

    s.r[7] = 0xFFFFFFFFu;
    CHECK(sr_cp0_mtc0(&s, 7u, SR_CP0_STATUS, 0u, TEST_BASE + 8u) == 0, "status write must succeed");
    CHECK(s.cop0[SR_CP0_STATUS] == (SR_STATUS_WRITABLE_MASK & 0xFFFFFFFFu),
          "status must be masked to writable bits (got 0x%08x)", s.cop0[SR_CP0_STATUS]);
    CHECK((s.cop0[SR_CP0_STATUS] & SR_STATUS_ERL) == 0u, "Status.ERL must not be writable");

    s.cop0[SR_CP0_CAUSE] = 0x0000FC00u;  /* hardware-pending bits, INTC-owned */
    s.r[8] = 0xFFFFFFFFu;
    CHECK(sr_cp0_mtc0(&s, 8u, SR_CP0_CAUSE, 0u, TEST_BASE + 12u) == 0, "cause write must succeed");
    CHECK(s.cop0[SR_CP0_CAUSE] == (0x0000FC00u | SR_CAUSE_IP_SW_MASK),
          "cause must keep HW pending, set SW pair (got 0x%08x)", s.cop0[SR_CP0_CAUSE]);

    s.r[9] = 0x12340000u;
    CHECK(sr_cp0_mtc0(&s, 9u, SR_CP0_COUNT, 0u, TEST_BASE + 16u) == 0, "count write must succeed");
    CHECK(sr_cp0_mfc0(&s, 10u, SR_CP0_COUNT, 0u, TEST_BASE + 20u) == 0, "count read must succeed");
    CHECK(s.r[10] == 0x12340000u, "count round-trip gave 0x%08x", s.r[10]);

    /* $zero semantics: mfc0 to $zero writes nothing, mtc0 from $zero moves zero. */
    s.r[11] = 0xAAAAAAAAu;
    CHECK(sr_cp0_mfc0(&s, 0u, SR_CP0_EPC, 0u, TEST_BASE + 24u) == 0, "mfc0 to $zero must succeed");
    CHECK(s.r[0] == 0u && s.r[11] == 0xAAAAAAAAu, "$zero move clobbered state");
    CHECK(sr_cp0_mtc0(&s, 0u, SR_CP0_EPC, 0u, TEST_BASE + 28u) == 0, "mtc0 from $zero must succeed");
    CHECK(s.cop0[SR_CP0_EPC] == 0u, "mtc0 from $zero must store zero");
    CHECK(s.flow_kind == SR_FLOW_NONE, "clean COP0 traffic must not set flow");
}

/* User-mode COP0 access traps; unsupported encodings raise RI. */
static void test_cop0_traps(void) {
    CpuState s;
    fresh_state(&s);
    s.cop0[SR_CP0_STATUS] = SR_STATUS_KSU_USER;
    s.r[5] = 1u;
    CHECK(sr_cp0_mtc0(&s, 5u, SR_CP0_STATUS, 0u, TEST_BASE) < 0, "user mtc0 must unwind");
    CHECK(s.flow_kind == SR_FLOW_EXCEPTION, "user mtc0 must trap");
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_CPU, "user mtc0 must trap as CPU");
    CHECK(s.cop0[SR_CP0_EPC] == TEST_BASE && (s.cop0[SR_CP0_CAUSE] & SR_CAUSE_BD) == 0u,
          "user trap needs exact EPC/BD");

    fresh_state(&s);
    s.cop0[SR_CP0_STATUS] = SR_STATUS_KSU_USER;
    CHECK(sr_cp0_mfc0(&s, 6u, SR_CP0_STATUS, 0u, TEST_BASE) < 0, "user mfc0 must unwind");
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_CPU, "user mfc0 must trap as CPU");

    /* Unsupported register/select combinations raise RI, never answer zero. */
    fresh_state(&s);
    CHECK(sr_cp0_mfc0(&s, 6u, 7u, 0u, TEST_BASE) < 0, "mfc0 rd=7 must unwind");
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_RI, "bad rd must raise RI");
    CHECK(s.r[6] == 0u, "failed mfc0 must not write its destination");

    fresh_state(&s);
    CHECK(sr_cp0_mfc0(&s, 6u, SR_CP0_STATUS, 1u, TEST_BASE) < 0, "mfc0 sel=1 must unwind");
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_RI, "bad sel must raise RI");

    fresh_state(&s);
    CHECK(sr_cp0_mtc0(&s, 5u, 20u, 0u, TEST_BASE) < 0, "mtc0 rd=20 must unwind");
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_RI, "bad mtc0 rd must raise RI");

    fresh_state(&s);
    s.r[5] = 0x11111111u;
    CHECK(sr_cp0_mtc0(&s, 5u, SR_CP0_PRID, 0u, TEST_BASE) < 0, "mtc0 PRID must unwind");
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_RI, "PRID write must raise RI");

    /* Delay-slot privilege faults name the branch (spec 3.3 BD rule). */
    fresh_state(&s);
    s.cop0[SR_CP0_STATUS] = SR_STATUS_KSU_USER;
    s.in_delay_slot = 1u;
    s.next_pc = TEST_BASE;
    CHECK(sr_cp0_mtc0(&s, 5u, SR_CP0_STATUS, 0u, TEST_BASE + 4u) < 0, "delay mtc0 must unwind");
    CHECK(s.cop0[SR_CP0_EPC] == TEST_BASE, "delay trap EPC must be the branch (got 0x%08x)",
          s.cop0[SR_CP0_EPC]);
    CHECK((s.cop0[SR_CP0_CAUSE] & SR_CAUSE_BD) != 0u, "delay trap must set BD");
}

/* Interpreter lane with the LLE gate on: syscall/break trap, eret returns,
 * COP0 round-trips; with the gate off the old UNSUPPORTED stands. */
static void test_interp_lane(void) {
    CpuState s;
    SrGuestInterpFault fault;
    SrGuestInterpResult r;

    fresh_state(&s);
    sr_cpu_lle_set_enabled(1);
    MEM_W32_PC(TEST_BASE + 0u, 0x0000000Cu, TEST_BASE);  /* syscall */
    MEM_W32_PC(TEST_BASE + 4u, 0x00000000u, TEST_BASE);  /* nop (never reached) */
    s.pc = TEST_BASE;
    r = sr_guest_interp_run(&s, TEST_BASE, &fault);
    CHECK(r == SR_GUEST_INTERP_EXCEPTION, "interp syscall must raise (got %s)",
          sr_guest_interp_result_name(r));
    CHECK(s.cop0[SR_CP0_EPC] == TEST_BASE, "interp EPC=0x%08x", s.cop0[SR_CP0_EPC]);
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_SYS, "interp ExcCode must be SYS");
    CHECK(s.pc == TEST_VECTOR && s.flow_target == TEST_VECTOR, "interp must transfer to vector");

    fresh_state(&s);
    sr_cpu_lle_set_enabled(1);
    MEM_W32_PC(TEST_BASE + 0u, 0x0000000Du, TEST_BASE);  /* break */
    s.pc = TEST_BASE;
    r = sr_guest_interp_run(&s, TEST_BASE, &fault);
    CHECK(r == SR_GUEST_INTERP_EXCEPTION, "interp break must raise (got %s)",
          sr_guest_interp_result_name(r));
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_BP, "interp ExcCode must be BP");

    /* eret with an owned EPC target. */
    fresh_state(&s);
    sr_cpu_lle_set_enabled(1);
    MEM_W32_PC(TEST_BASE + 0u, SR_OPCODE_ERET, TEST_BASE);
    s.pc = TEST_BASE;
    s.cop0[SR_CP0_STATUS] = SR_STATUS_EXL;
    s.cop0[SR_CP0_EPC] = TEST_BASE + 0x10u;
    MEM_W32_PC(TEST_BASE + 0x10u, 0x00000000u, TEST_BASE);  /* landing nop */
    r = sr_guest_interp_run(&s, TEST_BASE, &fault);
    CHECK(r == SR_GUEST_INTERP_ERET, "interp eret must report ERET (got %s)",
          sr_guest_interp_result_name(r));
    CHECK(s.pc == TEST_BASE + 0x10u, "interp eret pc=0x%08x", s.pc);
    CHECK((s.cop0[SR_CP0_STATUS] & SR_STATUS_EXL) == 0u, "interp eret must clear EXL");

    /* COP0 round-trip through the interpreter. */
    fresh_state(&s);
    sr_cpu_lle_set_enabled(1);
    MEM_W32_PC(TEST_BASE + 0u, enc_mtc0(5u, SR_CP0_EPC), TEST_BASE);
    MEM_W32_PC(TEST_BASE + 4u, enc_mfc0(6u, SR_CP0_EPC), TEST_BASE + 4u);
    MEM_W32_PC(TEST_BASE + 8u, 0x03E00008u, TEST_BASE + 8u);  /* jr $ra (returns) */
    MEM_W32_PC(TEST_BASE + 12u, 0x00000000u, TEST_BASE + 12u);
    s.pc = TEST_BASE;
    s.r[5] = 0x08810020u;
    s.r[31] = TEST_SPAN_END;  /* jr $ra lands outside any span: stops cleanly */
    r = sr_guest_interp_run(&s, TEST_BASE, &fault);
    CHECK(s.r[6] == 0x08810020u, "interp COP0 round-trip gave 0x%08x", s.r[6]);
    CHECK(s.flow_kind == SR_FLOW_NONE, "clean interp COP0 traffic must not set flow");
    (void)r;

    /* Gate off: the historical fail-closed contract stands. */
    fresh_state(&s);
    MEM_W32_PC(TEST_BASE + 0u, 0x0000000Cu, TEST_BASE);
    s.pc = TEST_BASE;
    r = sr_guest_interp_run(&s, TEST_BASE, &fault);
    CHECK(r == SR_GUEST_INTERP_UNSUPPORTED, "gate-off syscall must stay UNSUPPORTED (got %s)",
          sr_guest_interp_result_name(r));
    CHECK(fault.pc == TEST_BASE, "gate-off fault must name the probe word");
}

/* Config accessors round-trip and reset restores every default. */
static void test_config(void) {
    uint32_t g, i, b;
    sr_cpu_lle_reset_config();
    CHECK(!sr_cpu_lle_enabled(), "gate must default off");
    sr_cpu_lle_set_enabled(1);
    CHECK(sr_cpu_lle_enabled(), "gate enable must stick");
    sr_cpu_lle_set_vectors(1u, 2u, 3u);
    sr_cpu_lle_get_vectors(&g, &i, &b);
    CHECK(g == 1u && i == 2u && b == 3u, "vector accessors must round-trip");
    sr_cpu_lle_set_prid(0x12345678u);
    {
        CpuState s;
        fresh_state(&s);
        /* fresh_state resets config; re-apply after it. */
        sr_cpu_lle_set_prid(0x12345678u);
        CHECK(sr_cp0_mfc0(&s, 6u, SR_CP0_PRID, 0u, TEST_BASE) == 0, "PRID read must succeed");
        CHECK(s.r[6] == 0x12345678u, "PRID must report the configured value");
    }
    sr_cpu_lle_reset_config();
    sr_cpu_lle_get_vectors(&g, &i, &b);
    CHECK(g == 0x80000180u && !sr_cpu_lle_enabled(), "reset must restore defaults");
    CHECK(b == 0xBFC00200u, "reset must restore the bootstrap default");
    {
        CpuState s;
        fresh_state(&s);
        sr_cpu_clear_flow(&s);
        CHECK(s.flow_kind == SR_FLOW_NONE && s.flow_target == 0u, "clear_flow must zero the channel");
    }
}

static void test_nested_exceptions(void) {
    CpuState s;
    fresh_state(&s);
    /* First exception establishes EPC and Cause.BD. */
    s.cop0[SR_CP0_STATUS] = 0u;
    s.cop0[SR_CP0_EPC] = 0u;
    s.cop0[SR_CP0_CAUSE] = 0u;
    CHECK(sr_cpu_raise_exception(&s, SR_EXC_SYS, 0x08801004u, 0x08801000u, 0u, 1u, 0u) < 0,
          "first exception must unwind");
    CHECK(s.cop0[SR_CP0_EPC] == 0x08801000u, "first EPC must be branch pc");
    CHECK((s.cop0[SR_CP0_CAUSE] & SR_CAUSE_BD) != 0u, "first exception must set Cause.BD");
    CHECK((s.cop0[SR_CP0_STATUS] & SR_STATUS_EXL) != 0u, "first exception must set EXL");

    /* Second exception while Status.EXL is 1: nested exception.
     * MIPS32 spec Vol. III: EPC and Cause.BD must be preserved; Cause.ExcCode updated. */
    CHECK(sr_cpu_raise_exception(&s, SR_EXC_BP, 0x08802000u, 0x08802000u, 0u, 0u, 0u) < 0,
          "nested exception must unwind");
    CHECK(s.cop0[SR_CP0_EPC] == 0x08801000u, "nested exception must preserve EPC (got 0x%08x)", s.cop0[SR_CP0_EPC]);
    CHECK((s.cop0[SR_CP0_CAUSE] & SR_CAUSE_BD) != 0u, "nested exception must preserve Cause.BD");
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_BP, "nested exception must update Cause.ExcCode to BP");
}

static void test_unbacked_aot_target(void) {
    CpuState s;
    fresh_state(&s);
    /* 0x32200000 is an AOT module address outside guest RAM (not readable). */
    CHECK(!sr_guest_span_readable(0x32200000u, 4u), "0x32200000 must not be readable in RAM arena");
    sr_exec_span_register(0x32200000u, 0x32210000u);
    CHECK(sr_exec_span_owns_fetch(0x32200000u), "registered AOT span must own fetch");

    /* Exception vector pointing to unbacked AOT span must succeed, not SR_FLOW_FATAL. */
    sr_cpu_lle_set_vectors(0x32200000u, 0x32200000u, 0x32200000u);
    CHECK(sr_cpu_raise_exception(&s, SR_EXC_SYS, TEST_BASE, TEST_BASE, 0u, 0u, 0u) < 0,
          "raise exception to unbacked AOT vector must unwind");
    CHECK(s.flow_kind == SR_FLOW_EXCEPTION, "unbacked AOT vector must be EXCEPTION, got %u", s.flow_kind);
    CHECK(s.flow_target == 0x32200000u, "flow_target must be vector");

    /* eret targeting unbacked AOT span must succeed, not SR_FLOW_FATAL. */
    fresh_state(&s);
    s.cop0[SR_CP0_STATUS] = SR_STATUS_EXL;
    s.cop0[SR_CP0_EPC] = 0x32200000u;
    CHECK(sr_cpu_eret(&s, TEST_BASE) < 0, "eret to unbacked AOT target must unwind");
    CHECK(s.flow_kind == SR_FLOW_ERET, "unbacked AOT eret must be ERET, got %u", s.flow_kind);
    CHECK(s.flow_target == 0x32200000u, "flow_target must be EPC");
}

static void test_dispatch_interpreter_flow(void) {
    CpuState s;
    fresh_state(&s);
    sr_cpu_lle_set_enabled(1);

    /* TEST_BASE contains a syscall */
    MEM_W32_PC(TEST_BASE + 0u, 0x0000000Cu, TEST_BASE);  /* syscall */
    MEM_W32_PC(TEST_BASE + 4u, 0x00000000u, TEST_BASE);  /* nop */

    /* TEST_VECTOR contains handler code: v0 = 42; jr $ra; nop */
    MEM_W32_PC(TEST_VECTOR + 0u, 0x2402002Au, TEST_VECTOR);      /* addiu $v0, $zero, 42 */
    MEM_W32_PC(TEST_VECTOR + 4u, 0x03E00008u, TEST_VECTOR + 4u);  /* jr $ra */
    MEM_W32_PC(TEST_VECTOR + 8u, 0x00000000u, TEST_VECTOR + 8u);  /* nop */

    uint32_t resume_pc = TEST_BASE + 0x20u;
    MEM_W32_PC(resume_pc, 0x00000000u, resume_pc);

    s.pc = TEST_BASE;
    s.r[2] = 0u;
    s.r[31] = resume_pc;

    int r = dispatch_call_try(&s, TEST_BASE, resume_pc);
    CHECK(r == SR_GUEST_INTERP_CALL_RETURN, "dispatch_call_try must report CALL_RETURN (got %d)", r);
    CHECK(s.r[2] == 42u, "exception vector must execute and set v0=42 (got %u)", s.r[2]);
    CHECK(s.flow_kind == SR_FLOW_NONE, "flow metadata must be consumed/cleared (got %u)", s.flow_kind);
    CHECK(s.flow_target == 0u, "flow_target must be cleared");
}

static void test_interp_flow_trace(void) {
    CpuState s;
    SrGuestInterpFault fault;
    fresh_state(&s);
    sr_cpu_lle_set_enabled(1);

    /* User mode: mtc0 will raise CPU exception */
    s.cop0[SR_CP0_STATUS] = SR_STATUS_KSU_USER;
    MEM_W32_PC(TEST_BASE + 0u, enc_mtc0(5u, SR_CP0_EPC), TEST_BASE);
    s.pc = TEST_BASE;

    const char *trace_path = "build/mygame/test_interp_trace.txt";
    CHECK(sr_trace_open(trace_path, "test", TEST_BASE) == 0, "sr_trace_open must succeed");
    SrGuestInterpResult r = sr_guest_interp_run(&s, TEST_BASE, &fault);
    sr_trace_close();

    CHECK(r == SR_GUEST_INTERP_EXCEPTION, "user mtc0 must raise exception");

    /* Read trace file to verify that sr_end was called for the faulting instruction */
    FILE *f = fopen(trace_path, "rb");
    CHECK(f != NULL, "trace file must exist");
    char buf[1024] = {0};
    size_t bytes = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    CHECK(bytes > 0, "trace file must not be empty");
    /* The trace must contain the step record for the mtc0 instruction */
    CHECK(strstr(buf, "\n0 pc=0x08810000") != NULL, "trace must record the faulting instruction");

    /* Now test that a genuinely rejected opcode does NOT finish a trace record */
    fresh_state(&s);
    MEM_W32_PC(TEST_BASE + 0u, 0x00000005u, TEST_BASE);  /* unsupported opcode */
    s.pc = TEST_BASE;
    CHECK(sr_trace_open(trace_path, "test", TEST_BASE) == 0, "sr_trace_open must succeed");
    r = sr_guest_interp_run(&s, TEST_BASE, &fault);
    sr_trace_close();
    CHECK(r == SR_GUEST_INTERP_UNSUPPORTED, "unsupported opcode must return UNSUPPORTED");

    f = fopen(trace_path, "rb");
    CHECK(f != NULL, "trace file must exist");
    memset(buf, 0, sizeof(buf));
    bytes = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    CHECK(strstr(buf, "\n0 pc=0x08810000") == NULL, "rejected opcode must NOT record trace step");
}

/* PSP-A3-01 end to end: the measured AdEL must come out of an actual `lw`, not
 * only out of a direct sr_cpu_raise_exception() call. Before the data-access
 * check existed this load SUCCEEDED, because SR_PHYS() masks 0x88000014 down to
 * 0x08000014 and the range check accepted the alias. */
static void test_hw_psp_a3_01_adel_through_load(void) {
    CpuState s;
    SrGuestInterpFault fault;
    SrGuestInterpResult r;

    fresh_state(&s);
    sr_cpu_lle_set_enabled(1);
    MEM_W32_PC(TEST_BASE + 0u, enc_lw(13u, 14u, 4u), TEST_BASE);
    s.pc = TEST_BASE;
    s.cop0[SR_CP0_STATUS] = TEST_PRE_STATUS;   /* user mode, EXL clear */
    s.r[13] = 0x88000010u;
    s.r[14] = 0x0BADBABEu;                     /* sentinel destination */
    r = sr_guest_interp_run(&s, TEST_BASE, &fault);
    CHECK(r == SR_GUEST_INTERP_EXCEPTION,
          "user-mode kernel-segment lw must raise, not read RAM (got %s)",
          sr_guest_interp_result_name(r));
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_ADEL,
          "kernel-segment lw ExcCode must be 4 (AdEL)");
    CHECK(s.cop0[SR_CP0_EPC] == TEST_BASE, "kernel-segment lw EPC=0x%08x, want the lw address",
          s.cop0[SR_CP0_EPC]);
    CHECK(s.cop0[SR_CP0_BADVADDR] == 0x88000014u,
          "kernel-segment lw BadVAddr=0x%08x, want the effective address",
          s.cop0[SR_CP0_BADVADDR]);
    CHECK(s.r[14] == 0x0BADBABEu, "AdEL must leave the destination register unchanged");
    CHECK(s.pc == TEST_VECTOR, "kernel-segment lw must transfer to the vector");

    /* Kernel mode may reach kseg0: it is an unmapped mirror of physical RAM,
     * which is exactly what the masking accessor implements. */
    fresh_state(&s);
    sr_cpu_lle_set_enabled(1);
    MEM_W32_PC(TEST_BASE + 0u, enc_lw(13u, 14u, 4u), TEST_BASE);
    MEM_W32_PC(TEST_BASE + 4u, 0x03E00008u, TEST_BASE + 4u);  /* jr $ra */
    MEM_W32_PC(TEST_BASE + 8u, 0x00000000u, TEST_BASE + 8u);
    s.pc = TEST_BASE;
    s.cop0[SR_CP0_STATUS] = 0u;                /* kernel mode */
    s.r[13] = 0x88000010u;
    s.r[31] = TEST_SPAN_END;
    r = sr_guest_interp_run(&s, TEST_BASE, &fault);
    CHECK(r != SR_GUEST_INTERP_EXCEPTION,
          "kernel-mode kseg0 load must not fault (got %s)", sr_guest_interp_result_name(r));
    CHECK(s.flow_kind == SR_FLOW_NONE, "kernel-mode kseg0 load must not set flow");
}

/* Alignment is SYNTHETIC: the MIPS32 architectural rule, not a PSP measurement.
 * These pin the behaviour so a later probe has something to confirm or correct. */
static void test_lle_misaligned_data_access(void) {
    CpuState s;
    SrGuestInterpFault fault;
    SrGuestInterpResult r;

    fresh_state(&s);
    sr_cpu_lle_set_enabled(1);
    MEM_W32_PC(TEST_BASE + 0u, enc_lw(13u, 14u, 1u), TEST_BASE);
    s.pc = TEST_BASE;
    s.cop0[SR_CP0_STATUS] = TEST_PRE_STATUS;
    s.r[13] = TEST_BASE;
    s.r[14] = 0x0BADBABEu;
    r = sr_guest_interp_run(&s, TEST_BASE, &fault);
    CHECK(r == SR_GUEST_INTERP_EXCEPTION, "misaligned lw must raise (got %s)",
          sr_guest_interp_result_name(r));
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_ADEL,
          "misaligned load ExcCode must be 4 (AdEL)");
    CHECK(s.cop0[SR_CP0_BADVADDR] == TEST_BASE + 1u,
          "misaligned load BadVAddr=0x%08x", s.cop0[SR_CP0_BADVADDR]);
    CHECK(s.r[14] == 0x0BADBABEu, "misaligned load must not write the destination");

    /* A misaligned store is AdES (ExcCode 5), not AdEL. */
    fresh_state(&s);
    sr_cpu_lle_set_enabled(1);
    MEM_W32_PC(TEST_BASE + 0u, enc_sw(13u, 14u, 2u), TEST_BASE);
    s.pc = TEST_BASE;
    s.cop0[SR_CP0_STATUS] = TEST_PRE_STATUS;
    s.r[13] = TEST_BASE;
    s.r[14] = 0xDEADBEEFu;
    r = sr_guest_interp_run(&s, TEST_BASE, &fault);
    CHECK(r == SR_GUEST_INTERP_EXCEPTION, "misaligned sw must raise (got %s)",
          sr_guest_interp_result_name(r));
    CHECK(CAUSE_EXCCODE(s.cop0[SR_CP0_CAUSE]) == SR_EXC_ADES,
          "misaligned store ExcCode must be 5 (AdES)");
    CHECK(s.cop0[SR_CP0_BADVADDR] == TEST_BASE + 2u,
          "misaligned store BadVAddr=0x%08x", s.cop0[SR_CP0_BADVADDR]);

    /* A byte access has no alignment constraint. */
    CHECK(sr_cpu_data_access_fault(&s, TEST_BASE + 1u, 1u, 0) == 0u,
          "lb at an odd address must not fault");
}

/* The gate keeps default builds byte-identical: with LLE off, both cases keep
 * their historical fail-closed results and neither enters a guest vector. */
static void test_lle_gate_off_data_access(void) {
    CpuState s;
    SrGuestInterpFault fault;
    SrGuestInterpResult r;

    fresh_state(&s);
    MEM_W32_PC(TEST_BASE + 0u, enc_lw(13u, 14u, 1u), TEST_BASE);
    s.pc = TEST_BASE;
    s.cop0[SR_CP0_STATUS] = TEST_PRE_STATUS;
    s.r[13] = TEST_BASE;
    r = sr_guest_interp_run(&s, TEST_BASE, &fault);
    CHECK(r == SR_GUEST_INTERP_MISALIGNED_DATA,
          "gate-off misaligned lw must stay MISALIGNED_DATA (got %s)",
          sr_guest_interp_result_name(r));
    CHECK(s.flow_kind == SR_FLOW_NONE, "gate-off fault must not set flow");

    fresh_state(&s);
    MEM_W32_PC(TEST_BASE + 0u, enc_lw(13u, 14u, 4u), TEST_BASE);
    MEM_W32_PC(TEST_BASE + 4u, 0x03E00008u, TEST_BASE + 4u);  /* jr $ra */
    MEM_W32_PC(TEST_BASE + 8u, 0x00000000u, TEST_BASE + 8u);
    s.pc = TEST_BASE;
    s.cop0[SR_CP0_STATUS] = TEST_PRE_STATUS;
    s.r[13] = 0x88000010u;
    s.r[31] = TEST_SPAN_END;
    r = sr_guest_interp_run(&s, TEST_BASE, &fault);
    CHECK(r != SR_GUEST_INTERP_EXCEPTION,
          "gate-off kernel-segment lw must keep the historical masked read (got %s)",
          sr_guest_interp_result_name(r));
    CHECK(s.flow_kind == SR_FLOW_NONE, "gate-off kernel-segment lw must not set flow");
}

int main(void) {
    sr_mem_init();
    setup_spans();
    test_dispatch_interpreter_flow();
    test_interp_flow_trace();
    test_exception_entry_plain();
    test_exception_delay_slot();
    test_exception_ce_badvaddr();
    test_hw_psp_a1_01_break_plain();
    test_hw_psp_a2_01_break_delay_slot();
    test_hw_psp_a3_01_adel_load();
    test_hw_psp_a3_02_03_misaligned_codes_and_badvaddr();
    test_hw_psp_a3_01_adel_through_load();
    test_lle_misaligned_data_access();
    test_lle_gate_off_data_access();
    test_nested_exceptions();
    test_vector_selection();
    test_unbacked_aot_target();
    test_eret();
    test_mode_checks();
    test_cop0_roundtrip();
    test_cop0_traps();
    test_interp_lane();
    test_config();
    sr_cpu_lle_reset_config();
    if (g_failed) {
        fprintf(stderr, "cpu-lle selftest: FAILED\n");
        return 1;
    }
    printf("cpu-lle selftest: OK\n");
    return 0;
}
