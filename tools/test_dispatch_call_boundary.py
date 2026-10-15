# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Mutation proof for the production interpreter CALL/RETURN boundary.

Each mutant is applied only to a temporary copy of the production dispatch or
interpreter source.  The source-owned dispatch-isolation selftest must compile
cleanly and then fail on the semantic assertion; a compiler failure is reported
as a test failure rather than accepted as a kill.
"""

from __future__ import annotations

import os
import re
import shutil
import struct
import subprocess
import sys
import unittest
from pathlib import Path

try:
    import scratch_dir
except ModuleNotFoundError:
    from tools import scratch_dir


ROOT = Path(__file__).resolve().parent.parent
CC = shutil.which("gcc")
SELFTEST = ROOT / "src" / "rt" / "dispatch_isolation_selftest.c"
RECOMP = ROOT / "src" / "rt" / "recomp.c"
RECOMP_H = ROOT / "src" / "rt" / "recomp.h"
GUEST_INTERP = ROOT / "src" / "rt" / "guest_interp.c"
SR_CORO = ROOT / "src" / "rt" / "sr_coro.c"
PERF = ROOT / "src" / "rt" / "perf.c"
TITLE_CONFIG_TOOL = ROOT / "tools" / "title_runtime_config.py"
CODEGEN_TOOL = ROOT / "tools" / "codegen.py"


def _write_minimal_elf(
    path: Path, words: tuple[int, ...], *, base: int = 0x1000,
) -> None:
    """Write a source-owned ELF32 fixture with one executable load span."""
    payload_off = 52 + 32
    filesz = len(words) * 4
    blob = bytearray(payload_off + filesz)
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into(
        "<HHIIIIIHHHHHH", blob, 16,
        2, 8, 1, base, 52, 0, 0, 52, 32, 1, 0, 0, 0,
    )
    struct.pack_into(
        "<8I", blob, 52,
        1, payload_off, base, base, filesz, filesz, 5, 4,
    )
    for index, word in enumerate(words):
        struct.pack_into("<I", blob, payload_off + index * 4, word & 0xFFFFFFFF)
    path.write_bytes(blob)


def _build_and_run_stack_census(
    words: tuple[int, ...], expected_status: str, *,
    codegen_args: tuple[str, ...] = (), expected_final_sp: int = 0x2000,
    post_call_probe: str = "", extra_words: tuple[int, ...] | None = None,
    generated_body_prefix: str = "", codegen_mutation: tuple[str, str] | None = None,
) -> tuple[int, str, int, str]:
    """Generate and execute a real codegen entry wrapper against recomp.c."""
    assert CC is not None
    with scratch_dir.TemporaryDirectory(prefix="stack_census_pipeline_") as tmp:
        work = Path(tmp)
        codegen_tool = _mutated_copy(CODEGEN_TOOL, work, codegen_mutation)
        census_env = dict(os.environ)
        census_env["PYTHONPATH"] = str(ROOT / "tools") + os.pathsep + census_env.get("PYTHONPATH", "")
        elf = work / "fixture.elf"
        _write_minimal_elf(elf, words)
        generated = work / "census.c"
        extra_codegen_args = []
        if extra_words is not None:
            extra_elf = work / "extra.elf"
            _write_minimal_elf(extra_elf, extra_words, base=0)
            extra_codegen_args.append(f"--extra-elf={extra_elf}@0x2000")
        codegen = subprocess.run(
            [
                sys.executable, str(codegen_tool), str(elf), str(generated),
                "--base=0", "--profile=none", "--funcs-per-chunk=2000",
                "--stack-census", *codegen_args, *extra_codegen_args,
            ],
            cwd=ROOT, capture_output=True, text=True, env=census_env,
        )
        if codegen.returncode != 0:
            return codegen.returncode, codegen.stderr + codegen.stdout, 1, ""
        if generated_body_prefix:
            generated_body = work / "census_0.c"
            generated_text = generated_body.read_text(encoding="utf-8")
            marker = "static void _sr_stack_census_body_00001000(CpuState *s) {\n"
            if generated_text.count(marker) != 1:
                return 2, "frame-leak mutation did not find one generated callable body", 1, ""
            generated_body.write_text(
                generated_text.replace(marker, marker + generated_body_prefix, 1),
                encoding="utf-8",
                newline="\n",
            )

        config = subprocess.run(
            [sys.executable, str(TITLE_CONFIG_TOOL), "--output", str(work / "sr_title_config.h")],
            cwd=ROOT, capture_output=True, text=True,
        )
        if config.returncode != 0:
            return config.returncode, config.stderr + config.stdout, 1, ""

        harness = work / "stack_census_harness.c"
        harness.write_text(
            "#define main embedded_dispatch_isolation_main\n"
            "#include \"dispatch_isolation_selftest.c\"\n"
            "#undef main\n"
            "extern void sr_register_all(void);\n"
            "int main(void) {\n"
            "    CpuState s = {0};\n"
            "    s.r[29] = 0x2000u;\n"
            "    s.cop0[SR_CP0_EPC] = 0x00001000u;\n"
            "    sr_register_all();\n"
            "    if (sr_stack_census_status() != SR_STACK_CENSUS_NOT_OBSERVED) return 2;\n"
            "    RecompFn fn = sr_lookup(0x00001000u);\n"
            "    if (!fn) return 3;\n"
            "    fn(&s);\n"
            f"{post_call_probe}"
            "    sr_register_all();\n"
            "    SrStackCensusStatus status = sr_stack_census_status();\n"
            "    sr_stack_census_report();\n"
            f"    if (s.r[29] != 0x{expected_final_sp:08x}u) return 4;\n"
            "    if (status != EXPECTED_STACK_CENSUS_STATUS) return 5;\n"
            "    return 0;\n"
            "}\n".replace(
                "EXPECTED_STACK_CENSUS_STATUS", "SR_STACK_CENSUS_EXPECTED_STATUS"
            ),
            encoding="utf-8",
            newline="\n",
        )
        # The harness status is selected per test case through a compile-time define.
        exe = work / "stack_census_harness.exe"
        compile_result = subprocess.run(
            [
                CC, "-std=c11", "-O0", "-fno-strict-aliasing",
                "-Wall", "-Wextra", "-DSR_SDL3VK", "-D_CRT_SECURE_NO_WARNINGS",
                "-DSR_STACK_CENSUS_ENABLED",
                f"-DSR_STACK_CENSUS_EXPECTED_STATUS={expected_status}",
                "-I", str(work), "-I", str(ROOT / "src" / "rt"),
                str(harness), str(generated), str(work / "census_0.c"),
                str(GUEST_INTERP), str(PERF),
                str(ROOT / "src" / "rt" / "sr_coro.c"),
                str(ROOT / "src" / "rt" / "cpu_lle.c"),
                str(ROOT / "src" / "rt" / "domain_mode.c"),
                str(ROOT / "src" / "rt" / "stale_code.c"),
                str(ROOT / "src" / "rt" / "title_config.c"),
                str(ROOT / "src" / "rt" / "vfpu_tables.c"),
                "-lm", "-o", str(exe),
            ],
            cwd=ROOT, capture_output=True, text=True,
        )
        if compile_result.returncode != 0:
            return compile_result.returncode, compile_result.stderr + compile_result.stdout, 1, ""
        run_result = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True)
        return 0, compile_result.stderr + compile_result.stdout, run_result.returncode, run_result.stderr + run_result.stdout


# --- generated-code call depth on a guest fiber ----------------------------------
# A tiny Allegrex encoder for source-owned fixtures. Registers: zero 0, v0 2, a0 4,
# t9 25, sp 29, ra 31.
def _i(op: int, rs: int, rt: int, imm: int) -> int:
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def _addiu(rt: int, rs: int, imm: int) -> int: return _i(0x09, rs, rt, imm)
def _ori(rt: int, rs: int, imm: int) -> int: return _i(0x0D, rs, rt, imm)
def _lui(rt: int, imm: int) -> int: return _i(0x0F, 0, rt, imm)
def _sw(rt: int, off: int, base: int) -> int: return _i(0x2B, base, rt, off)
def _lw(rt: int, off: int, base: int) -> int: return _i(0x23, base, rt, off)
def _beq(rs: int, rt: int, here: int, target: int) -> int:
    return _i(0x04, rs, rt, (target - (here + 4)) >> 2)
def _j(target: int) -> int: return (0x02 << 26) | ((target >> 2) & 0x03FFFFFF)
def _jal(target: int) -> int: return (0x03 << 26) | ((target >> 2) & 0x03FFFFFF)
def _jr(rs: int) -> int: return (rs << 21) | 0x08
def _addu(rd: int, rs: int, rt: int) -> int: return (rs << 21) | (rt << 16) | (rd << 11) | 0x21


_NOP = 0
_V0, _V1, _A0, _T9, _SP, _RA = 2, 3, 4, 25, 29, 31
_PROBE_BASE = 0x1000
_PROBE_SP = 0x09F00000


def _probe_program(body: list[int]) -> tuple[int, ...]:
    """Entry at 0x1000 links into the cell at 0x1010 and returns to the harness.

    The return's delay slot copies $sp to $v1, so the SP the call left behind is
    observable before the entry body's own entry-SP restore masks it.
    """
    entry = [_jal(0x1010), _NOP, _jr(_RA), _addu(_V1, _SP, 0)]
    return tuple(entry + body)


def _direct_tail_cycle() -> tuple[int, ...]:
    """A and B hand control to each other with `j`: guest call depth stays at one."""
    a, done, b = 0x1010, 0x1024, 0x102C
    body = [
        _beq(_A0, 0, a, done), _NOP,          # 0x1010
        _addiu(_A0, _A0, -1), _j(b), _NOP,     # 0x1018 .. 0x1020
        _jr(_RA), _NOP,                        # 0x1024 done
        _j(a), _addiu(_V0, _V0, 1),            # 0x102C B (count in the slot)
    ]
    return _probe_program(body)


def _computed_tail_cycle() -> tuple[int, ...]:
    """The same cycle through `jr $t9`, the shape of a computed tail transfer."""
    a, done, b = 0x1010, 0x102C, 0x1034
    body = [
        _beq(_A0, 0, a, done), _NOP,                           # 0x1010
        _addiu(_A0, _A0, -1), _lui(_T9, 0), _ori(_T9, _T9, b),  # 0x1018 .. 0x1020
        _jr(_T9), _NOP,                                        # 0x1024
        _jr(_RA), _NOP,                                        # 0x102C done
        _lui(_T9, 0), _ori(_T9, _T9, a),                       # 0x1034 B
        _jr(_T9), _addiu(_V0, _V0, 1),
    ]
    return _probe_program(body)


def _tails_per_call_recursion(tails: int) -> tuple[int, ...]:
    """Valid recursion whose every level reaches the next through `tails` direct tails.

    A: if (a0 == 0) return; push ra; a0--; jal T1; pop ra; return.
    T1 -> ... -> Tk each `j` onward and the last `j A`, so a level is one CALL plus
    `tails` TAIL transfers. Native depth must follow the CALLs alone.
    """
    a, done = 0x1010, 0x1034
    first_tail = 0x103C
    call_target = first_tail if tails else a
    body = [
        _beq(_A0, 0, a, done), _NOP,                       # 0x1010
        _addiu(_SP, _SP, -8), _sw(_RA, 0, _SP),             # 0x1018
        _addiu(_A0, _A0, -1),                              # 0x1020
        _jal(call_target), _addiu(_V0, _V0, 1),            # 0x1024
        _lw(_RA, 0, _SP), _addiu(_SP, _SP, 8),              # 0x102C
        _jr(_RA), _NOP,                                    # 0x1034 done
    ]
    for index in range(tails):
        here = first_tail + 8 * index
        onward = here + 8 if index + 1 < tails else a
        body += [_j(onward), _NOP]
        assert here == _PROBE_BASE + 4 * (4 + len(body) - 2)
    return _probe_program(body)


_CHAIN_MARK = 0x5A5A


def _entry_tail_program() -> tuple[int, ...]:
    """The entry itself ends in a direct TAIL, so whoever invoked it must drain.

    0x1000 `j 0x1010` (the delay slot copies $sp to $v1); 0x1008 is a dead `jal` that only
    makes 0x1010 a translated body; 0x1010 sets v0 = 7 and returns to the harness.
    """
    return (
        _j(0x1010), _addu(_V1, _SP, 0),           # 0x1000 entry: tail to 0x1010
        _jal(0x1010), _NOP,                       # 0x1008 discovery anchor (dead)
        _addiu(_V0, 0, 7), _jr(_RA), _NOP,        # 0x1010
    )


def _segment_chain() -> tuple[int, ...]:
    """C1 -> computed H -> direct X -> computed F, with $sp lowered by C1 and by H.

    H's direct tail to X used to nest inside H's dispatch, and X's computed tail to F nested
    inside X: F ran at the live SP (two frames down) and wrote it to v0. Only after F
    returned did X, H and C1 restore in turn, leaving C1's entry SP for the caller.
    """
    c1, h, x, f = 0x1010, 0x102C, 0x1040, 0x1058
    body = [
        _addiu(_SP, _SP, -16), _lui(_T9, 0), _ori(_T9, _T9, h),   # 0x1010 C1
        _jr(_T9), _NOP,                                           # 0x101C
        _jr(_RA), _NOP,                                           # 0x1024 anchor
        _addiu(_SP, _SP, -16), _j(x), _NOP,                       # 0x102C H
        _jr(_RA), _NOP,                                           # 0x1038 anchor
        _lui(_T9, 0), _ori(_T9, _T9, f), _jr(_T9), _NOP,          # 0x1040 X
        _jr(_RA), _NOP,                                           # 0x1050 anchor
        _addu(_V0, _SP, 0), _jr(_RA), _NOP,                       # 0x1058 F
    ]
    assert c1 == 0x1010 and _PROBE_BASE + 4 * (4 + 18) == f
    return _probe_program(body)


def _frame_sharing_tail() -> tuple[int, ...]:
    """C1 opens a frame, stores a mark in it and tails to C2 without its epilogue.

    C2 runs in C1's frame (a jump-table case body does exactly this) and reads the mark
    through $sp. The chain then ends, and the owner's entry SP is restored afterwards.
    """
    c1, c2 = 0x1010, 0x102C
    body = [
        _addiu(_SP, _SP, -16),                             # 0x1010 C1
        _addiu(_T9, 0, _CHAIN_MARK), _sw(_T9, 4, _SP),      # 0x1014
        _j(c2), _NOP,                                      # 0x101C
        _jr(_RA), _NOP,                                    # 0x1024 (anchor for C2)
        _NOP,                                              # 0x1028
        _lw(_V0, 4, _SP),                                  # 0x102C C2: read C1's slot
        _jr(_RA), _NOP,                                    # 0x1030
    ]
    assert c1 == 0x1010
    return _probe_program(body)


def _three_body_chain() -> tuple[int, ...]:
    """C1 -> C2 -> C3, each tail taken with a different $sp; C3 returns.

    The nested returns restored C3's, then C2's, then C1's entry SP; only C1's survives,
    so the caller sees its own SP back. C2 and C3 count into v0.
    """
    c1, c2, c3 = 0x1010, 0x1024, 0x1038
    body = [
        _addiu(_SP, _SP, -16), _j(c2), _NOP,               # 0x1010 C1
        _jr(_RA), _NOP,                                    # 0x101C anchor
        _addiu(_SP, _SP, -16), _j(c3),                     # 0x1024 C2
        _addiu(_V0, _V0, 1),                               # 0x102C slot
        _jr(_RA), _NOP,                                    # 0x1030 anchor
        _addiu(_V0, _V0, 2), _jr(_RA), _NOP,               # 0x1038 C3
    ]
    assert c1 == 0x1010
    return _probe_program(body)


def _linked_recursion() -> tuple[int, ...]:
    """Control: a valid recursive `jal` that saves `$ra` on the guest stack.

    Guest call depth really grows with the iteration count, so native depth must too.
    """
    a, done = 0x1010, 0x1034
    body = [
        _beq(_A0, 0, a, done), _NOP,                       # 0x1010
        _addiu(_SP, _SP, -8), _sw(_RA, 0, _SP),             # 0x1018
        _addiu(_A0, _A0, -1),                              # 0x1020
        _jal(a), _addiu(_V0, _V0, 1),                      # 0x1024
        _lw(_RA, 0, _SP), _addiu(_SP, _SP, 8),              # 0x102C
        _jr(_RA), _NOP,                                    # 0x1034 done
    ]
    return _probe_program(body)


def _interpreted_caller_of_compiled_callee() -> tuple[int, ...]:
    """C (interpreted when omitted) calls compiled leaf L, then adds 5 to v0."""
    leaf = 0x1034
    body = [
        _addiu(_SP, _SP, -8), _sw(_RA, 0, _SP),            # 0x1010 C
        _jal(leaf), _NOP,                                  # 0x1018
        _addiu(_V0, _V0, 5),                               # 0x1020 continuation
        _lw(_RA, 0, _SP), _addiu(_SP, _SP, 8),              # 0x1024
        _jr(_RA), _NOP,                                    # 0x102C
        _jr(_RA), _addiu(_V0, _V0, 1),                     # 0x1034 L
    ]
    return _probe_program(body)


# #968: a linked conditional branch (bal is bgezal $zero; bltzal) whose target is
# another module's code, so it is never a label of the caller's body. The caller keeps
# the issue's shape and makes each effect countable: the slot adds 100, the leaf adds
# 1, the continuation at the link adds 10 and copies the live $ra into a0.
_MODULE_BASE = 0x2000
_BAL, _BLTZAL = 0x11, 0x10
_LEAF = (_addiu(_V0, _V0, 1), _jr(_RA), _NOP)


def _regimm(selector: int, rs: int, here: int, target: int) -> int:
    return _i(0x01, rs, selector, (target - (here + 4)) >> 2)


def _linked_caller(at: int, selector: int, callee: int) -> tuple[int, ...]:
    return (
        _addiu(_SP, _SP, -16), _sw(_RA, 0, _SP),           # at
        _regimm(selector, 0, at + 8, callee),              # at + 8: bal / bltzal $zero
        _addiu(_V0, _V0, 100),                             # delay slot: once per path
        _addiu(_V0, _V0, 10),                              # at + 16: the link
        _addu(_A0, _RA, 0),
        _lw(_RA, 0, _SP), _jr(_RA), _addiu(_SP, _SP, 16),
    )


def _linked_branch_into_module(selector: int) -> tuple[tuple[int, ...], tuple[int, ...]]:
    """The primary cell at 0x1010 links to a native leaf that a second module owns."""
    return _probe_program(list(_linked_caller(0x1010, selector, _MODULE_BASE))), _LEAF


def _linked_branch_into_interpreted_body() -> tuple[tuple[int, ...], tuple[int, ...]]:
    """The module's cell links back to a primary leaf at 0x1010 omitted from AOT."""
    primary = (_jal(_MODULE_BASE), _NOP, _jr(_RA), _addu(_V1, _SP, 0), *_LEAF)
    return primary, _linked_caller(_MODULE_BASE, _BAL, 0x1010)


_DEPTH_PROBE_HARNESS = r"""
#define sr_yield isolation_sr_yield
#define main embedded_dispatch_isolation_main
#include "dispatch_isolation_selftest.c"
#undef main
#undef sr_yield
#include <stdio.h>
extern void sr_register_all(void);

/* Every generated body begins with SR_YIELD; with a timeslice of one it calls
 * sr_yield at each guest function entry, which samples the fiber's remaining
 * host stack. The probe is the production stack-bounds query. */
static size_t g_probe_min = (size_t)-1;
static unsigned long g_probe_samples;
void sr_yield(CpuState *s) {
    size_t remaining;
    (void)s;
#ifdef SR_TAIL_DRAIN
    /* Production sr_yield refuses a live tail transfer (#855); so does the probe. */
    sr_tail_assert_empty("tail-transfer-live-at-yield");
#endif
    if (sr_coro_stack_remaining(&remaining)) {
        if (remaining < g_probe_min) g_probe_min = remaining;
        g_probe_samples++;
    }
    atomic_store_explicit(&sr_timeslice, 1, memory_order_relaxed);
}

static SrCoro *g_probe_main;
static CpuState g_probe_state;
static size_t g_probe_top;
static int g_probe_ran;

static void probe_body(void *arg) {
    (void)arg;
    RecompFn fn = sr_lookup(0x00001000u);
    if (sr_coro_stack_remaining(&g_probe_top) && fn) {
#ifdef SR_TAIL_DRAIN
        sr_call_body(fn, &g_probe_state);   /* complete the entry's TAIL chain (#855) */
#else
        fn(&g_probe_state);                 /* runtimes before the TAIL protocol */
#endif
        g_probe_ran = 1;
    }
    sr_coro_switch(g_probe_main);
    for (;;) sr_coro_switch(g_probe_main);
}

/* "pair" / "pair-reverse": two guest-size fibers, created A then B. The first to run makes a
 * shallow recursion, which leaves the host-stack window measured on its own stack (#855);
 * the other then runs n levels. Across both orders the second fiber lies above the first in
 * one of them, so a window that bounded only the low end would pass its frames there. */
static unsigned long g_pair_n[2];
static int g_pair_ran[2];
static void pair_body(void *arg) {
    const int i = arg ? 1 : 0;   /* A is created with a NULL argument, B with a non-NULL one */
    RecompFn fn = sr_lookup(0x00001000u);
    memset(&g_probe_state, 0, sizeof g_probe_state);
    g_probe_state.r[29] = PROBE_SP;
    g_probe_state.r[4] = (uint32_t)g_pair_n[i];
    if (fn) {
#ifdef SR_TAIL_DRAIN
        sr_call_body(fn, &g_probe_state);
#else
        fn(&g_probe_state);
#endif
        g_pair_ran[i] = 1;
    }
    sr_coro_switch(g_probe_main);
    for (;;) sr_coro_switch(g_probe_main);
}

int main(int argc, char **argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    unsigned long n = argc > 1 ? strtoul(argv[1], NULL, 0) : 0ul;
    sr_mem_init();
    /* The loader's job: the interpreter tier executes guest memory, so the image
     * must be there, exactly as the translated bodies were generated from it. */
    static const uint32_t image[] = { PROBE_WORDS };
    for (size_t i = 0; i < sizeof image / sizeof image[0]; i++)
        MEM_W32(PROBE_BASE + 4u * (uint32_t)i, image[i]);
    sr_register_all();
    memset(&g_probe_state, 0, sizeof g_probe_state);
    g_probe_state.r[29] = PROBE_SP;
    g_probe_state.r[4] = (uint32_t)n;
    const int neighbour = argc > 2 && strcmp(argv[2], "neighbour") == 0;
    const int pair = argc > 2 && strncmp(argv[2], "pair", 4) == 0;
    /* "no-bounds": the scheduler is on but no stack-bounds query is installed. */
    const int no_bounds = argc > 2 && strcmp(argv[2], "no-bounds") == 0;
#ifdef SR_TAIL_DRAIN
    if (argc > 2 && !neighbour && !pair && !no_bounds) {   /* a broken protocol (#855) must stop with its named fault */
        CpuState other;
        memset(&other, 0, sizeof other);
        if (strcmp(argv[2], "recorded-twice") == 0) {
            sr_tail_record(&g_probe_state, 0x00001010u, NULL, 0u, 0u);
            sr_tail_record(&g_probe_state, 0x00001010u, NULL, 0u, 0u);
        } else if (strcmp(argv[2], "live-at-dispatch") == 0) {
            sr_tail_record(&g_probe_state, 0x00001010u, NULL, 0u, 0u);
            dispatch(&g_probe_state, 0x00001010u);
        } else if (strcmp(argv[2], "foreign-state") == 0) {
            sr_tail_record(&other, 0x00001010u, NULL, 0u, 0u);
            sr_tail_drain(&g_probe_state);
        }
        return 30;   /* reaching here means the fault did not stop the run */
    }
#endif
    g_probe_main = sr_coro_main();
    if (!g_probe_main) return 20;
    if (!no_bounds) {
        sr_dispatch_host_stack_remaining = sr_coro_stack_remaining;
#ifdef SR_HOST_STACK_CHECK
        sr_dispatch_host_stack_top = sr_coro_stack_top;   /* as sched_init installs it (#855) */
#endif
    }
    sr_sched_on = 1;
    atomic_store_explicit(&sr_timeslice, 1, memory_order_relaxed);
    if (pair) {
        SrCoro *a = sr_coro_create(pair_body, NULL, SR_GUEST_FIBER_STACK_SIZE);
        SrCoro *b = sr_coro_create(pair_body, &g_pair_ran[1], SR_GUEST_FIBER_STACK_SIZE);
        if (!a || !b) return 24;
        const int reverse = strcmp(argv[2], "pair-reverse") == 0;
        g_pair_n[reverse ? 1 : 0] = 1000ul;   /* the first fiber to run: shallow */
        g_pair_n[reverse ? 0 : 1] = n;        /* then the other: n levels */
        sr_coro_switch(reverse ? b : a);
        sr_coro_switch(reverse ? a : b);
        sr_sched_on = 0;
        if (!g_pair_ran[0] || !g_pair_ran[1]) return 22;
        printf("DEPTH_PROBE n=%lu v0=%u sp=0x%08x\n",
               n, (unsigned)g_probe_state.r[2], (unsigned)g_probe_state.r[29]);
        return 0;
    }
    SrCoro *fiber = sr_coro_create(probe_body, NULL, SR_GUEST_FIBER_STACK_SIZE);
    if (!fiber) return 21;
    /* "neighbour": a second guest-size stack created after the probe's, as the scheduler
     * creates one per guest thread. POSIX mmap is top-down, so it usually lands directly
     * below the probe's stack, where an overrun of the probe's stack would write. */
    if (neighbour && !sr_coro_create(probe_body, NULL, SR_GUEST_FIBER_STACK_SIZE)) return 23;
    sr_coro_switch(fiber);
    sr_sched_on = 0;
    sr_dispatch_host_stack_remaining = NULL;
    if (!g_probe_ran) return 22;
    printf("DEPTH_PROBE n=%lu native_bytes=%zu samples=%lu v0=%u v1=0x%08x a0=%u sp=0x%08x\n",
           n, g_probe_top > g_probe_min ? g_probe_top - g_probe_min : (size_t)0,
           g_probe_samples, (unsigned)g_probe_state.r[2], (unsigned)g_probe_state.r[3],
           (unsigned)g_probe_state.r[4], (unsigned)g_probe_state.r[29]);
    return 0;
}
"""


def _mutated_copy(source: Path, work: Path, mutation: tuple[str, str] | None) -> Path:
    """A scratch copy of ``source`` with one exact replacement, or the source itself."""
    if mutation is None:
        return source
    old, new = mutation
    text = source.read_text(encoding="utf-8")
    if text.count(old) != 1:
        raise AssertionError(f"mutation anchor drifted in {source.name}: {old[:60]!r}")
    copy = work / source.name
    copy.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    return copy


def _build_depth_probe(
    words: tuple[int, ...], work: Path, *, codegen_args: tuple[str, ...] = (),
    mutate: dict[str, tuple[str, str]] | None = None,
) -> tuple[Path | None, str]:
    """Translate a source-owned fixture and link it into the depth-probe harness.

    ``mutate`` maps ``codegen.py``, ``recomp.c``, ``recomp.h``, ``guest_interp.c`` or
    ``sr_coro.c`` to one exact replacement applied to a scratch copy, so a mutant is
    generated, compiled and run through the same production path as the unmutated build.
    """
    assert CC is not None
    mutate = mutate or {}
    unknown = set(mutate) - {"codegen.py", "recomp.c", "recomp.h", "guest_interp.c", "sr_coro.c"}
    if unknown:
        raise AssertionError(f"unsupported mutation target(s): {sorted(unknown)}")
    elf = work / "fixture.elf"
    _write_minimal_elf(elf, words, base=_PROBE_BASE)
    generated = work / "probe.c"
    codegen_tool = _mutated_copy(CODEGEN_TOOL, work, mutate.get("codegen.py"))
    env = dict(os.environ)
    # A mutated codegen.py copy still imports its siblings from tools/.
    env["PYTHONPATH"] = str(ROOT / "tools") + os.pathsep + env.get("PYTHONPATH", "")
    codegen = subprocess.run(
        [
            sys.executable, str(codegen_tool), str(elf), str(generated),
            "--base=0", "--profile=none", "--funcs-per-chunk=2000", *codegen_args,
        ],
        cwd=ROOT, capture_output=True, text=True, env=env,
    )
    if codegen.returncode != 0:
        return None, codegen.stderr + codegen.stdout
    config = subprocess.run(
        [sys.executable, str(TITLE_CONFIG_TOOL), "--output", str(work / "sr_title_config.h")],
        cwd=ROOT, capture_output=True, text=True,
    )
    if config.returncode != 0:
        return None, config.stderr + config.stdout
    harness = work / "depth_probe_harness.c"
    harness.write_text(
        _DEPTH_PROBE_HARNESS.replace("PROBE_SP", f"0x{_PROBE_SP:08x}u")
        .replace("PROBE_BASE", f"0x{_PROBE_BASE:08x}u")
        .replace("PROBE_WORDS", ", ".join(f"0x{w & 0xFFFFFFFF:08x}u" for w in words)),
        encoding="utf-8", newline="\n",
    )
    if "recomp.c" in mutate or "recomp.h" in mutate:
        # The harness reaches recomp.c and recomp.h through dispatch_isolation_selftest.c's
        # quoted includes, which search the including file's directory first; copy them.
        if "recomp.c" in mutate:
            _mutated_copy(RECOMP, work, mutate["recomp.c"])
        else:   # recomp.c's own quoted include of recomp.h must find the mutated copy
            (work / RECOMP.name).write_text(
                RECOMP.read_text(encoding="utf-8"), encoding="utf-8", newline="\n"
            )
        if "recomp.h" in mutate:
            _mutated_copy(RECOMP_H, work, mutate["recomp.h"])
        (work / SELFTEST.name).write_text(
            SELFTEST.read_text(encoding="utf-8"), encoding="utf-8", newline="\n"
        )
    guest_interp = _mutated_copy(GUEST_INTERP, work, mutate.get("guest_interp.c"))
    sr_coro = _mutated_copy(SR_CORO, work, mutate.get("sr_coro.c"))
    exe = work / "depth_probe.exe"
    compile_result = subprocess.run(
        [
            CC, "-std=c11", "-O0", "-fno-strict-aliasing",
            "-Wall", "-Wextra", "-DSR_SDL3VK", "-D_CRT_SECURE_NO_WARNINGS",
            "-I", str(work), "-I", str(ROOT / "src" / "rt"),
            str(harness), str(generated), str(work / "probe_0.c"),
            str(guest_interp), str(PERF),
            str(sr_coro),
            str(ROOT / "src" / "rt" / "cpu_lle.c"),
            str(ROOT / "src" / "rt" / "domain_mode.c"),
            str(ROOT / "src" / "rt" / "stale_code.c"),
            str(ROOT / "src" / "rt" / "title_config.c"),
            str(ROOT / "src" / "rt" / "vfpu_tables.c"),
            "-lm", "-o", str(exe),
        ],
        cwd=ROOT, capture_output=True, text=True,
    )
    if compile_result.returncode != 0:
        return None, compile_result.stderr + compile_result.stdout
    return exe, compile_result.stderr + compile_result.stdout


def _run_depth_probe(exe: Path, iterations: int) -> tuple[int, str, dict[str, int]]:
    run = subprocess.run(
        [str(exe), str(iterations)], cwd=ROOT, capture_output=True, text=True, timeout=120,
    )
    fields: dict[str, int] = {}
    for line in run.stdout.splitlines():
        if line.startswith("DEPTH_PROBE "):
            for item in line.split()[1:]:
                key, _, value = item.partition("=")
                fields[key] = int(value, 0)
    return run.returncode, run.stdout + run.stderr, fields


def _build_and_run(mutated_recomp: str | None = None,
                   mutated_interp: str | None = None) -> tuple[int, str, int, str]:
    """Build and run the real selftest against temporary source copies."""
    assert CC is not None
    with scratch_dir.TemporaryDirectory(prefix="dispatch_call_boundary_mut_") as tmp:
        work = Path(tmp)
        (work / "dispatch_isolation_selftest.c").write_text(
            SELFTEST.read_text(encoding="utf-8"), encoding="utf-8", newline="\n"
        )
        (work / "recomp.c").write_text(
            mutated_recomp if mutated_recomp is not None
            else RECOMP.read_text(encoding="utf-8"),
            encoding="utf-8", newline="\n",
        )
        interp_path = work / "guest_interp.c"
        interp_path.write_text(
            mutated_interp if mutated_interp is not None
            else GUEST_INTERP.read_text(encoding="utf-8"),
            encoding="utf-8", newline="\n",
        )
        config = subprocess.run(
            [sys.executable, str(TITLE_CONFIG_TOOL), "--output", str(work / "sr_title_config.h")],
            cwd=ROOT, capture_output=True, text=True,
        )
        if config.returncode != 0:
            return config.returncode, config.stderr + config.stdout, 1, ""

        exe = work / "dispatch_isolation_selftest.exe"
        compile_result = subprocess.run(
            [
                CC, "-std=c11", "-O0", "-fno-strict-aliasing",
                "-Wall", "-Wextra", "-DSR_SDL3VK", "-D_CRT_SECURE_NO_WARNINGS",
                "-I", str(work), "-I", str(ROOT / "src" / "rt"),
                 str(work / "dispatch_isolation_selftest.c"), str(interp_path),
                 str(ROOT / "src" / "rt" / "sr_coro.c"),
                 str(PERF),
                 str(ROOT / "src" / "rt" / "cpu_lle.c"),
                str(ROOT / "src" / "rt" / "domain_mode.c"),
                str(ROOT / "src" / "rt" / "stale_code.c"),
                str(ROOT / "src" / "rt" / "title_config.c"),
                str(ROOT / "src" / "rt" / "vfpu_tables.c"),
                "-lm", "-o", str(exe),
            ],
            cwd=ROOT, capture_output=True, text=True,
        )
        if compile_result.returncode != 0:
            return compile_result.returncode, compile_result.stderr + compile_result.stdout, 1, ""
        run_result = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True)
        return 0, compile_result.stderr + compile_result.stdout, run_result.returncode, run_result.stderr + run_result.stdout


@unittest.skipUnless(CC, "gcc is required for the compiled mutation proof")
class DispatchCallBoundaryMutationTests(unittest.TestCase):
    """The CALL contract must be load-bearing, not just source decoration."""

    def assert_killed(self, name: str, *, recomp_old: str | None = None,
                      recomp_new: str | None = None,
                      interp_old: str | None = None,
                      interp_new: str | None = None,
                      diagnostic: str) -> None:
        original_recomp = RECOMP.read_text(encoding="utf-8")
        original_interp = GUEST_INTERP.read_text(encoding="utf-8")
        if recomp_old is not None:
            self.assertIsNotNone(recomp_new)
            self.assertIn(recomp_old, original_recomp, f"{name}: recomp mutation anchor drifted")
            mutated_recomp = original_recomp.replace(recomp_old, recomp_new, 1)
        else:
            mutated_recomp = None
        if interp_old is not None:
            self.assertIsNotNone(interp_new)
            self.assertIn(interp_old, original_interp, f"{name}: interpreter mutation anchor drifted")
            mutated_interp = original_interp.replace(interp_old, interp_new, 1)
        else:
            mutated_interp = None

        compile_rc, compile_output, run_rc, run_output = _build_and_run(
            mutated_recomp=mutated_recomp, mutated_interp=mutated_interp
        )
        self.assertEqual(
            compile_rc, 0,
            f"{name}: MUTANT_BUILD_FAILED (not a semantic kill)\n{compile_output}",
        )
        self.assertNotEqual(
            run_rc, 0,
            f"{name}: MUTANT_SURVIVED the production selftest\n{run_output}",
        )
        self.assertIn(
            diagnostic, run_output,
            f"{name}: failure did not identify the intended boundary semantic\n{run_output}",
        )
        print(f"{name}: MUTANT_EXECUTED_AND_SEMANTIC_TEST_FAILED")

    def test_pristine_production_selftest_passes(self):
        compile_rc, compile_output, run_rc, run_output = _build_and_run()
        self.assertEqual(compile_rc, 0, compile_output)
        self.assertEqual(run_rc, 0, run_output)
        self.assertIn("dispatch-isolation-selftest: OK", run_output)

    def test_late_import_cache_mutation_survives_registry_retirement(self):
        anchor = "                target = resolved;  /* use resolved target for logging below */"
        self.assert_killed(
            "late-import-persistent-alias",
            recomp_old=anchor,
            recomp_new="                sr_register(target, fn);\n" + anchor,
            diagnostic="retired late import executed a stale body or changed guest state",
        )

    def test_M1_untyped_interpreter_dispatch_reexecutes_native_continuation(self):
        self.assert_killed(
            "M1-untyped-dispatch",
            recomp_old=(
                "SrGuestInterpResult interp_result = call_boundary\n"
                "        ? sr_guest_interp_run_with_boundary(s, target, call_boundary, &fault)\n"
                "        : sr_guest_interp_run(s, target, &fault);"
            ),
            recomp_new="SrGuestInterpResult interp_result = sr_guest_interp_run(s, target, &fault);",
            diagnostic="CALL boundary handed the interpreted callee through the native outer return",
        )

    def test_M2_resume_boundary_one_instruction_early_skips_return(self):
        self.assert_killed(
            "M2-early-resume",
            interp_old="if (boundary && instruction_count != 0u && pc == boundary->resume_pc) {",
            interp_new="if (boundary && instruction_count != 0u && pc == boundary->resume_pc - 4u) {",
            diagnostic="CALL frame/outer return state was not restored exactly",
        )

    def test_M3_resume_boundary_one_instruction_late_executes_continuation(self):
        self.assert_killed(
            "M3-late-resume",
            interp_old="if (boundary && instruction_count != 0u && pc == boundary->resume_pc) {",
            interp_new="if (boundary && instruction_count != 0u && pc == boundary->resume_pc + 4u) {",
            diagnostic="AOT continuation observed wrong caller-saved/store state",
        )

    def test_M4_live_ra_instead_of_explicit_boundary_is_wrong(self):
        self.assert_killed(
            "M4-live-ra-boundary",
            interp_old="if (boundary && instruction_count != 0u && pc == boundary->resume_pc) {",
            interp_new="if (boundary && instruction_count != 0u && pc == s->r[31]) {",
            diagnostic="AOT continuation observed wrong caller-saved/store state",
        )

    def test_M5_return_delay_slot_is_skipped(self):
        self.assert_killed(
            "M5-skip-return-delay",
            # The interpreter reports each instruction's guest store back to its
            # caller (so sr_end() can record it in the canonical trace), so this
            # call carries the store out-params too. The mutation is unchanged:
            # feed the slot a nop instead of the word that was fetched for it.
            interp_old=(
                "SrGuestInterpResult delay_result = execute_noncontrol(\n"
                "                s, pc + 4u, delay_opcode, &delay_store_address, "
                "&delay_store_size, fault);"
            ),
            interp_new=(
                "SrGuestInterpResult delay_result = execute_noncontrol(\n"
                "                s, pc + 4u, 0x24000000u, &delay_store_address, "
                "&delay_store_size, fault);"
            ),
            diagnostic="return delay slot did not execute exactly once",
        )

    def test_M6_return_delay_slot_executes_twice(self):
        self.assert_killed(
            "M6-duplicate-return-delay",
            interp_old=(
                "            instruction_count += 2u;\n"
                "            if (sr_perf_enabled) sr_perf_interp_instruction();\n"
                "            if (sr_perf_enabled) sr_perf_interp_instruction();\n"
            ),
            interp_new=(
                "            (void)execute_noncontrol(s, pc + 4u, delay_opcode,\n"
                "                                     &delay_store_address, "
                "&delay_store_size, fault);\n"
                "            instruction_count += 2u;\n"
                "            if (sr_perf_enabled) sr_perf_interp_instruction();\n"
                "            if (sr_perf_enabled) sr_perf_interp_instruction();\n"
            ),
            diagnostic="return delay slot did not execute exactly once",
        )


@unittest.skipUnless(CC, "gcc is required for the generated stack-census proof")
class StackCensusPipelineTests(unittest.TestCase):
    """Generated callable wrappers must measure real CpuState stack values."""

    def test_generated_balanced_leaf_reports_complete(self):
        compile_rc, compile_output, run_rc, run_output = _build_and_run_stack_census(
            (0x03E00008, 0x00000000),
            "SR_STACK_CENSUS_COMPLETE",
        )
        self.assertEqual(compile_rc, 0, compile_output)
        self.assertEqual(run_rc, 0, run_output)
        self.assertIn("STACK_CENSUS status=COMPLETE entries=1 returns=1", run_output)

    def test_generated_stack_leak_reports_failure(self):
        compile_rc, compile_output, run_rc, run_output = _build_and_run_stack_census(
            (0x27BDFFF0, 0x03E00008, 0x00000000),
            "SR_STACK_CENSUS_FAILED",
        )
        self.assertEqual(compile_rc, 0, compile_output)
        self.assertEqual(run_rc, 0, run_output)
        self.assertIn("STACK_CENSUS status=FAILED", run_output)
        self.assertIn("mismatches=1", run_output)

    def test_generated_frame_leak_mutant_is_caught(self):
        compile_rc, compile_output, run_rc, run_output = _build_and_run_stack_census(
            (0x03E00008, 0x00000000),
            "SR_STACK_CENSUS_FAILED",
            generated_body_prefix="    s->r[29] -= 16u; /* injected frame leak */\n",
        )
        self.assertEqual(compile_rc, 0, compile_output)
        self.assertEqual(run_rc, 0, run_output)
        self.assertIn("STACK_CENSUS status=FAILED", run_output)
        self.assertIn("mismatches=1", run_output)

    def test_generated_fallthrough_stack_leak_reports_failure(self):
        compile_rc, compile_output, run_rc, run_output = _build_and_run_stack_census(
            (0x27BDFFF0, 0x00000000),
            "SR_STACK_CENSUS_FAILED",
        )
        self.assertEqual(compile_rc, 0, compile_output)
        self.assertEqual(run_rc, 0, run_output)
        self.assertIn("STACK_CENSUS status=FAILED", run_output)
        self.assertIn("mismatches=1", run_output)

    def test_generated_eret_preserves_guest_sp_and_reports_partial(self):
        compile_rc, compile_output, run_rc, run_output = _build_and_run_stack_census(
            (0x27BDFFE0, 0x42000018),
            "SR_STACK_CENSUS_PARTIAL",
            codegen_args=("--lle-cpu",),
            expected_final_sp=0x1FE0,
        )
        self.assertEqual(compile_rc, 0, compile_output)
        self.assertEqual(run_rc, 0, run_output)
        self.assertIn("STACK_CENSUS status=PARTIAL entries=1 returns=1 excluded=1", run_output)

    def test_unexpected_entry_is_counted_once_per_invocation(self):
        # One unexpected invocation (enter + exit, as the generated wrapper does)
        # must read as unexpected=1, not one count per census hook.
        compile_rc, compile_output, run_rc, run_output = _build_and_run_stack_census(
            (0x03E00008, 0x00000000),
            "SR_STACK_CENSUS_PARTIAL",
            post_call_probe=(
                "    sr_stack_census_enter(0x00009990u);\n"
                "    sr_stack_census_exit(0x00009990u, 0x2000u, 0x2000u, 0u);\n"
                "    SrStackCensusSummary probe;\n"
                "    sr_stack_census_snapshot(&probe);\n"
                "    if (probe.unexpected != 1u) return 6;\n"
            ),
        )
        self.assertEqual(compile_rc, 0, compile_output)
        self.assertEqual(run_rc, 0, run_output)
        self.assertIn(
            "STACK_CENSUS status=PARTIAL entries=2 returns=2 excluded=0 unexpected=1",
            run_output,
        )

    # 0x1000 `j 0x1010` is the entry body's TAIL; the `jr ra` anchor at 0x1008 ends that body,
    # so 0x1010 (v0 = 7; return) is its own translated body. The entry's census wrapper must
    # drain the transfer before its census exit.
    _TAIL_WORDS = (
        0x08000404, 0x00000000,               # 0x1000 j 0x1010
        0x03E00008, 0x00000000,               # 0x1008 anchor
        0x24020007, 0x03E00008, 0x00000000,   # 0x1010
    )
    _TAIL_PROBE = ('    if (s.r[2] != 7u) return 6;\n'
                   '    sr_tail_assert_empty("census-probe");\n')

    def test_generated_wrapper_completes_its_body_tail_chain(self):
        # #855: a census body records its TAIL and its wrapper drains it before census exit.
        compile_rc, compile_output, run_rc, run_output = _build_and_run_stack_census(
            self._TAIL_WORDS, "SR_STACK_CENSUS_COMPLETE", post_call_probe=self._TAIL_PROBE,
        )
        self.assertEqual(compile_rc, 0, compile_output)
        self.assertEqual(run_rc, 0, run_output)

    def test_census_wrapper_that_does_not_drain_is_caught(self):
        compile_rc, compile_output, run_rc, run_output = _build_and_run_stack_census(
            self._TAIL_WORDS, "SR_STACK_CENSUS_COMPLETE", post_call_probe=self._TAIL_PROBE,
            codegen_mutation=('            "    SR_TAIL_DRAIN(s);\\n"\n'
                              '            f"    sr_stack_census_exit(',
                              '            f"    sr_stack_census_exit('),
        )
        self.assertEqual(compile_rc, 0, compile_output)
        self.assertEqual(run_rc, 6, f"MUTANT_SURVIVED rc={run_rc}\n{run_output}")
        print("C1-census-wrapper-does-not-drain: MUTANT_EXECUTED_AND_SEMANTIC_TEST_FAILED")

    def test_unobserved_generated_callable_reports_partial(self):
        compile_rc, compile_output, run_rc, run_output = _build_and_run_stack_census(
            (0x03E00008, 0x00000000),
            "SR_STACK_CENSUS_PARTIAL",
            extra_words=(0x03E00008, 0x00000000),
        )
        self.assertEqual(compile_rc, 0, compile_output)
        self.assertEqual(run_rc, 0, run_output)
        self.assertIn(
            "STACK_CENSUS status=PARTIAL entries=1 returns=1 excluded=0 unexpected=0",
            run_output,
        )
        self.assertIn("unobserved=1", run_output)


@unittest.skipUnless(CC, "gcc is required for the generated depth probe")
class GeneratedCallDepthTests(unittest.TestCase):
    """Generated code on a production-size guest fiber, measured at each guest entry."""

    def _probe(self, words, iterations, *, codegen_args=()):
        with scratch_dir.TemporaryDirectory(prefix="depth_probe_") as tmp:
            work = Path(tmp)
            exe, output = _build_depth_probe(words, work, codegen_args=codegen_args)
            self.assertIsNotNone(exe, output)
            generated = (work / "probe_0.c").read_text(encoding="utf-8")
            results = [_run_depth_probe(exe, n) for n in iterations]
        return generated, results

    def test_linked_recursion_native_depth_tracks_guest_depth(self):
        # Control for the probe itself: a valid recursion really nests guest calls,
        # so the measured native depth must grow with it.
        _generated, results = self._probe(_linked_recursion(), (16, 2048))
        for (rc, output, fields), n in zip(results, (16, 2048), strict=True):
            self.assertEqual(rc, 0, output)
            self.assertEqual(fields.get("v0"), n, output)
            self.assertEqual(fields.get("a0"), 0, output)
            self.assertEqual(fields.get("v1"), _PROBE_SP, output)
        growth = results[1][2]["native_bytes"] - results[0][2]["native_bytes"]
        self.assertGreater(growth, 2032 * 16, f"probe did not see real nesting: {results}")

    # --- #855: TAIL transfers return to a drain instead of nesting native calls ---------

    _TAIL_BOUND = 16 * 1024   # native growth allowed from n=16 to n=2048 for a tail cycle

    def _assert_cycle(self, results, iterations):
        for (rc, output, fields), n in zip(results, iterations, strict=True):
            self.assertEqual(rc, 0, output)
            self.assertEqual(fields.get("v0"), n, output)
            self.assertEqual(fields.get("a0"), 0, output)
            self.assertEqual(fields.get("v1"), _PROBE_SP, output)

    def test_direct_tail_cycle_runs_in_constant_native_depth(self):
        # A <-> B through `j` at guest call depth one. Before #855 each transfer nested a
        # native call (2,256 B at n=16, 262,352 B at n=2048 on Windows/gcc -O0).
        _generated, results = self._probe(_direct_tail_cycle(), (16, 2048))
        self._assert_cycle(results, (16, 2048))
        growth = results[1][2]["native_bytes"] - results[0][2]["native_bytes"]
        self.assertLess(growth, self._TAIL_BOUND, f"direct tail transfers nest: {results}")

    def test_computed_tail_cycle_runs_in_constant_native_depth(self):
        # The same cycle through `jr $t9`; before #855 each transfer cost a whole
        # dispatch chain (~544 B), the shape issue #855 measured at ~516 B.
        _generated, results = self._probe(_computed_tail_cycle(), (16, 2048))
        self._assert_cycle(results, (16, 2048))
        growth = results[1][2]["native_bytes"] - results[0][2]["native_bytes"]
        self.assertLess(growth, self._TAIL_BOUND, f"computed tail transfers nest: {results}")

    def test_long_tail_cycles_complete(self):
        # Before #855: 300,000 computed transfers stop at host-fiber-stack-exhausted and
        # 600,000 direct ones overflow the fiber outright (no preflight on that path).
        for words, n in ((_computed_tail_cycle(), 300_000), (_direct_tail_cycle(), 600_000)):
            with self.subTest(n=n):
                _generated, results = self._probe(words, (n,))
                self._assert_cycle(results, (n,))

    def test_native_depth_follows_calls_not_tail_transfers(self):
        # A recursion d levels deep where each level reaches the next through t direct
        # tails: native depth must not depend on t (before #855: 131,280 B for t=1 and
        # 262,352 B for t=3 at d=1024).
        depth = 1024
        measured = {}
        for tails in (1, 3):
            _generated, results = self._probe(_tails_per_call_recursion(tails), (depth,))
            rc, output, fields = results[0]
            self.assertEqual(rc, 0, output)
            self.assertEqual(fields.get("v0"), depth, output)
            self.assertEqual(fields.get("v1"), _PROBE_SP, output)
            measured[tails] = fields["native_bytes"]
        self.assertLess(abs(measured[3] - measured[1]), 4096, f"depth follows tails: {measured}")

    def test_frame_sharing_tail_keeps_the_frame_until_the_chain_ends(self):
        # C2 runs inside C1's frame; the owner's entry SP is restored only afterwards.
        _generated, results = self._probe(_frame_sharing_tail(), (0,))
        rc, output, fields = results[0]
        self.assertEqual(rc, 0, output)
        self.assertEqual(fields.get("v0"), _CHAIN_MARK, output)
        self.assertEqual(fields.get("v1"), _PROBE_SP, output)

    def test_tail_chain_restores_the_outermost_callable_entry_sp(self):
        _generated, results = self._probe(_three_body_chain(), (0,))
        rc, output, fields = results[0]
        self.assertEqual(rc, 0, output)
        self.assertEqual(fields.get("v0"), 3, output)
        self.assertEqual(fields.get("v1"), _PROBE_SP, output)

    def _assert_mutant_fails(self, name, words, iterations, mutate, check):
        with scratch_dir.TemporaryDirectory(prefix="depth_mutant_") as tmp:
            work = Path(tmp)
            exe, output = _build_depth_probe(words, work, mutate=mutate)
            self.assertIsNotNone(exe, f"{name}: MUTANT_BUILD_FAILED (not a semantic kill)\n{output}")
            results = [_run_depth_probe(exe, n) for n in iterations]
        self.assertTrue(check(results), f"{name}: MUTANT_SURVIVED {results}")
        print(f"{name}: MUTANT_EXECUTED_AND_SEMANTIC_TEST_FAILED")

    def test_tail_protocol_mutants_fail_for_their_semantic(self):
        # The last recorded restore wins instead of the first: the caller sees C2's SP.
        self._assert_mutant_fails(
            "T1-last-restore-wins", _three_body_chain(), (0,),
            {"guest_interp.c": ("if (next.has_restore && !chain_restores) {",
                                "if (next.has_restore) {")},
            lambda r: r[0][0] == 0 and r[0][2].get("v1") != _PROBE_SP,
        )
        # The owner's SP is restored at the TAIL site, before the target runs in its frame.
        self._assert_mutant_fails(
            "T2-restore-before-target", _frame_sharing_tail(), (0,),
            {"codegen.py": (
                'return f"sr_tail_record(s, {target_expr}, {fn}, _sp_entry, 1u); return;{suffix}"',
                'return f"s->r[29] = _sp_entry; sr_tail_record(s, {target_expr}, {fn}, 0u, 0u); return;{suffix}"',
            )},
            lambda r: r[0][0] == 0 and r[0][2].get("v0") != _CHAIN_MARK,
        )
        # A direct CALL site stops draining: the callee's transfer is left recorded.
        self._assert_mutant_fails(
            "T3-call-site-does-not-drain", _tails_per_call_recursion(1), (4,),
            {"codegen.py": ('return f"{symbol}(s); {BODY_CALL_DRAIN}"',
                            'return f"{symbol}(s);"')},
            lambda r: r[0][0] != 0 or r[0][2].get("v0") != 4 or r[0][2].get("sp") != _PROBE_SP,
        )
        # A computed step drains the whole chain: one native drain per computed transfer.
        self._assert_mutant_fails(
            "T4-drain-step-drains", _computed_tail_cycle(), (16, 2048),
            {"recomp.c": ("dispatch_try_with_boundary(s, target, NULL, SR_DRAIN_SEGMENT) < 0) {",
                          "dispatch_try_with_boundary(s, target, NULL, SR_DRAIN_CHAIN) < 0) {")},
            lambda r: all(x[0] == 0 for x in r)
            and r[1][2]["native_bytes"] - r[0][2]["native_bytes"] >= self._TAIL_BOUND,
        )

    def test_runtime_direct_call_completes_the_entry_tail_chain(self):
        # sr_call_body is how the driver, funcdiff and thread start invoke a body; an entry
        # that ends in a TAIL completes only if that call drains.
        _generated, results = self._probe(_entry_tail_program(), (0,))
        rc, output, fields = results[0]
        self.assertEqual(rc, 0, output)
        self.assertEqual(fields.get("v0"), 7, output)
        self.assertEqual(fields.get("v1"), _PROBE_SP, output)

    def test_computed_step_keeps_the_live_sp_for_the_next_computed_target(self):
        _generated, results = self._probe(_segment_chain(), (0,))
        rc, output, fields = results[0]
        self.assertEqual(rc, 0, output)
        self.assertEqual(fields.get("v0"), _PROBE_SP - 32, f"F did not run two frames down\n{output}")
        self.assertEqual(fields.get("v1"), _PROBE_SP, output)

    def test_segment_and_runtime_call_mutants_fail_for_their_semantic(self):
        # A runtime direct call stops draining: the entry's transfer is never run.
        self._assert_mutant_fails(
            "T5-call-body-does-not-drain", _entry_tail_program(), (0,),
            {"recomp.h": ("    fn(s);\n    if (sr_tail_transfer.pending) sr_tail_drain(s);\n",
                          "    fn(s);\n")},
            lambda r: r[0][0] != 0 or r[0][2].get("v0") != 7,
        )
        # A computed step applies its restore while the next computed transfer is pending:
        # F runs in H's frame instead of the live one.
        self._assert_mutant_fails(
            "T6-segment-restores-early", _segment_chain(), (0,),
            {"guest_interp.c": ("                sr_tail_transfer.has_restore = 1u;\n"
                                "            }\n            return;",
                                "                sr_tail_transfer.has_restore = 1u;\n"
                                "            }\n            if (chain_restores) s->r[29] = chain_sp;\n"
                                "            return;")},
            lambda r: r[0][0] == 0 and r[0][2].get("v0") != _PROBE_SP - 32,
        )

    def test_tail_protocol_faults_stop_with_their_names(self):
        with scratch_dir.TemporaryDirectory(prefix="tail_faults_") as tmp:
            exe, output = _build_depth_probe(_entry_tail_program(), Path(tmp))
            self.assertIsNotNone(exe, output)
            for mode in ("recorded-twice", "live-at-dispatch", "foreign-state"):
                with self.subTest(fault=mode):
                    run = subprocess.run([str(exe), "0", mode], cwd=ROOT, capture_output=True,
                                         text=True, timeout=120)
                    self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
                    expected = {"recorded-twice": "tail-transfer-recorded-twice",
                                "live-at-dispatch": "tail-transfer-live-at-dispatch",
                                "foreign-state": "tail-transfer-foreign-state"}[mode]
                    self.assertIn(f"TAIL_TRANSFER_FAULT: {expected}", run.stderr)

    def test_interpreted_caller_resumes_after_compiled_callee(self):
        # The caller is omitted from AOT, so the production interpreter runs it; its
        # `jal` lands on a registered native leaf. The guest expects the caller to
        # continue at the link address (v0 += 5) and unwind its own frame.
        generated, results = self._probe(
            _interpreted_caller_of_compiled_callee(), (0,),
            codegen_args=("--omit-aot=0x00001010",),
        )
        self.assertNotIn("void f_00001010(CpuState *s) {", generated,
                         "the caller must be interpreted for this cell to mean anything")
        self.assertIn("void f_00001034(CpuState *s) {", generated)
        rc, output, fields = results[0]
        self.assertEqual(rc, 0, output)
        self.assertEqual(fields.get("v0"), 6, f"interpreted caller lost its continuation\n{output}")
        # The caller unwound its own frame before returning (base: 8 bytes low).
        self.assertEqual(fields.get("v1"), _PROBE_SP, output)

    # --- #855 residual: direct CALL depth stops at the dispatcher's named boundary -----
    #
    # A `jal` to a known body is a native call that never enters the dispatcher, so before
    # the entry check a deep direct recursion ran off the 64 MiB fiber: Windows stopped with
    # STATUS_STACK_OVERFLOW (0xC00000FD), Linux with SIGSEGV, or, with another guest stack
    # mapped directly below (the scheduler's shape), wrote into it and finished "normally".

    _DEEP_LINKED = 3_000_000      # overruns the fiber at > 22 B per level (gcc -O0: 48-64 B)
    _SHALLOW_LINKED = 500_000     # 24-32 MB at gcc -O0: well inside the fiber
    # An overrun that ends inside a 64 MiB neighbour for 40-64 B per level (1.68M-2.1M).
    _NEIGHBOUR_OVERRUN = 1_900_000
    _NAMED_STOP_EVENT = ("BOOT_EVENT phase=stop reason=semantic-boundary "
                         "boundary=host-fiber-stack-exhausted issue=855")
    _NO_ENTRY_CHECK = ('return f"    SR_HOST_STACK_CHECK({entry_expr});"', 'return ""')

    @staticmethod
    def _run_probe_mode(exe, work, n, mode=None):
        """Run the probe with a boot-event file; returns (rc, stdout, stderr, events)."""
        events = work / f"events_{n}_{mode or 'plain'}.txt"
        env = dict(os.environ)
        env["SR_BOOT_EVENT_FILE"] = str(events)
        run = subprocess.run([str(exe), str(n), *([mode] if mode else [])], cwd=ROOT,
                             capture_output=True, text=True, timeout=120, env=env)
        text = events.read_text(encoding="utf-8") if events.exists() else ""
        return run.returncode, run.stdout, run.stderr, text

    def _assert_named_stop(self, result):
        rc, stdout, stderr, events = result
        detail = f"rc={rc}\n{stdout}\n{stderr}"
        self.assertEqual(rc, 1, detail)
        self.assertNotIn("DEPTH_PROBE", stdout, detail)
        self.assertIn("SEMANTIC_BOUNDARY: host-fiber-stack-exhausted;", stderr, detail)
        self.assertEqual(events.splitlines(), [self._NAMED_STOP_EVENT], detail)

    def test_deep_direct_recursion_stops_at_the_named_boundary(self):
        with scratch_dir.TemporaryDirectory(prefix="direct_depth_") as tmp:
            work = Path(tmp)
            exe, output = _build_depth_probe(_linked_recursion(), work)
            self.assertIsNotNone(exe, output)
            # The bound is the dispatcher's: a recursion that fits still runs to the end.
            rc, run_output, fields = _run_depth_probe(exe, self._SHALLOW_LINKED)
            self.assertEqual(rc, 0, run_output)
            self.assertEqual(fields.get("v0"), self._SHALLOW_LINKED, run_output)
            self.assertEqual(fields.get("sp"), _PROBE_SP, run_output)
            for mode in (None, "neighbour"):
                with self.subTest(mode=mode or "alone"):
                    self._assert_named_stop(
                        self._run_probe_mode(exe, work, self._DEEP_LINKED, mode))

    def test_unknown_stack_bounds_stop_a_direct_call_as_they_stop_a_dispatch(self):
        # Fail closed: with the scheduler on and no way to measure the stack, the first
        # checked entry stops at the dispatcher's other named host-stack boundary instead of
        # running unbounded (before the check this run finished with rc 0).
        with scratch_dir.TemporaryDirectory(prefix="direct_bounds_") as tmp:
            work = Path(tmp)
            exe, output = _build_depth_probe(_linked_recursion(), work)
            self.assertIsNotNone(exe, output)
            rc, stdout, stderr, events = self._run_probe_mode(exe, work, 16, "no-bounds")
        detail = f"rc={rc}\n{stdout}\n{stderr}"
        self.assertEqual(rc, 1, detail)
        self.assertNotIn("DEPTH_PROBE", stdout, detail)
        self.assertIn("SEMANTIC_BOUNDARY: host-fiber-stack-bounds-unavailable;", stderr, detail)
        self.assertEqual(events.splitlines(), [
            "BOOT_EVENT phase=stop reason=semantic-boundary "
            "boundary=host-fiber-stack-bounds-unavailable issue=855"], detail)

    def test_only_bodies_that_make_a_direct_call_check_their_stack(self):
        # Every native recursion passes through a direct CALL or the dispatcher, which
        # preflights; leaves, tail-only bodies and dispatched callers stay unchecked.
        cells = (
            ("linked", _linked_recursion(), (), {0x1000: True, 0x1010: True}),
            ("direct tails", _direct_tail_cycle(), (),
             {0x1000: True, 0x1010: False, 0x102C: False}),
            ("computed tails", _computed_tail_cycle(), (),
             {0x1000: True, 0x1010: False, 0x1034: False}),
            ("dispatched caller", _interpreted_caller_of_compiled_callee(),
             ("--omit-aot=0x00001010",), {0x1000: False, 0x1034: False}),
        )
        for name, words, codegen_args, expected in cells:
            with self.subTest(cell=name):
                generated, results = self._probe(words, (0,), codegen_args=codegen_args)
                self.assertEqual(results[0][0], 0, results[0][1])
                for entry, checked in expected.items():
                    head = f"void f_{entry:08x}(CpuState *s) {{\n"
                    self.assertEqual(generated.count(head), 1, f"{name}: no body 0x{entry:08x}")
                    opens_with_check = (
                        head + f"    SR_HOST_STACK_CHECK(0x{entry:08x}u);\n"
                        + f"    SR_YIELD(s, 0x{entry:08x}u);\n") in generated
                    self.assertEqual(opens_with_check, checked, f"{name}: body 0x{entry:08x}")
                self.assertEqual(generated.count("SR_HOST_STACK_CHECK("),
                                 sum(expected.values()), name)
                # The general rule, over every body, whatever emitted the call.
                for body in re.split(r"(?m)^(?=void [A-Za-z_]\w*\(CpuState \*s\) \{$)", generated):
                    head = re.match(r"void ([A-Za-z_]\w*)\(CpuState \*s\) \{\n", body)
                    if not head:
                        continue
                    calls = re.search(r"\b[A-Za-z_]\w*\(s\); SR_TAIL_DRAIN\(s\);", body)
                    opens = body[head.end():].startswith("    SR_HOST_STACK_CHECK(")
                    self.assertEqual(bool(calls), opens, f"{name}: {head.group(1)}")

    def test_direct_call_stack_check_mutants_fail_for_their_semantic(self):
        # Each mutant must overrun the fiber with no named stop, the shape before the check.
        def overran(results):
            rc, output, _fields = results[0]
            return rc not in (0, 1) and "host-fiber-stack-exhausted" not in output

        mutants = (
            ("D1-codegen-emits-no-entry-check", {"codegen.py": self._NO_ENTRY_CHECK}),
            ("D2-slow-path-ignores-the-measurement", {"recomp.c": (
                "int result = dispatch_host_stack_measure(&remaining);\n    if (result < 0) {",
                "int result = dispatch_host_stack_measure(&remaining);\n    if (result < -1000) {",
            )}),
            ("D3-refilled-window-keeps-no-reserve", {"recomp.c": (
                "uintptr_t low = here - remaining + SR_DISPATCH_STACK_SAFETY_MARGIN;",
                "uintptr_t low = here - remaining;",
            )}),
            ("D4-window-test-inverted", {"recomp.h": (
                "if (__builtin_expect(_sr_frame - sr_host_stack_window.low > "
                "sr_host_stack_window.span, 0))",
                "if (__builtin_expect(_sr_frame - sr_host_stack_window.low < "
                "sr_host_stack_window.span, 0))",
            )}),
        )
        for name, mutate in mutants:
            with self.subTest(mutant=name):
                self._assert_mutant_fails(name, _linked_recursion(), (self._DEEP_LINKED,),
                                          mutate, overran)

    @staticmethod
    def _probe_fields(stdout):
        for line in stdout.splitlines():
            if line.startswith("DEPTH_PROBE "):
                return {k: int(v, 0) for k, _, v in
                        (item.partition("=") for item in line.split()[1:])}
        return {}

    def test_a_window_measured_on_another_fiber_never_passes_its_frames(self):
        # The window caches one stack's safe range and is not cleared at a fiber switch:
        # another live stack lies wholly outside it, so its first checked entry refills it.
        with scratch_dir.TemporaryDirectory(prefix="pair_depth_") as tmp:
            work = Path(tmp)
            exe, output = _build_depth_probe(_linked_recursion(), work)
            self.assertIsNotNone(exe, output)
            for mode in ("pair", "pair-reverse"):
                with self.subTest(order=mode):
                    rc, stdout, stderr, _events = self._run_probe_mode(
                        exe, work, self._SHALLOW_LINKED, mode)
                    self.assertEqual(rc, 0, f"{stdout}\n{stderr}")
                    self.assertEqual(self._probe_fields(stdout).get("v0"),
                                     self._SHALLOW_LINKED, stdout)
                    self._assert_named_stop(
                        self._run_probe_mode(exe, work, self._DEEP_LINKED, mode))

    def test_window_without_a_top_mutant_fails_for_its_semantic(self):
        # Bounding only the low end lets the window measured on the lower fiber pass every
        # frame of the higher one, which then overruns its own stack unchecked.
        no_top = ("    sr_host_stack_window.span = top - low;",
                  "    sr_host_stack_window.span = UINTPTR_MAX - low;")
        with scratch_dir.TemporaryDirectory(prefix="depth_mutant_") as tmp:
            work = Path(tmp)
            exe, output = _build_depth_probe(_linked_recursion(), work,
                                             mutate={"recomp.c": no_top})
            self.assertIsNotNone(exe, f"D5-window-without-a-top: MUTANT_BUILD_FAILED (not a "
                                      f"semantic kill)\n{output}")
            results = {mode: self._run_probe_mode(exe, work, self._DEEP_LINKED, mode)
                       for mode in ("pair", "pair-reverse")}
        overran = [mode for mode, (rc, _out, err, _ev) in results.items()
                   if rc not in (0, 1) and "host-fiber-stack-exhausted" not in err]
        self.assertTrue(overran, f"D5-window-without-a-top: MUTANT_SURVIVED {results}")
        print("D5-window-without-a-top: MUTANT_EXECUTED_AND_SEMANTIC_TEST_FAILED")

    def _overrun_with_neighbour(self, mutate):
        """Overrun the fiber with the entry check removed and a guest stack created after it.

        Returns (faulted, detail): faulted means the run died without a named stop and
        without finishing, as a hit guard does.
        """
        with scratch_dir.TemporaryDirectory(prefix="guard_band_") as tmp:
            work = Path(tmp)
            exe, output = _build_depth_probe(_linked_recursion(), work, mutate=mutate)
            self.assertIsNotNone(exe, f"MUTANT_BUILD_FAILED (not a semantic result)\n{output}")
            rc, stdout, stderr, _events = self._run_probe_mode(
                exe, work, self._NEIGHBOUR_OVERRUN, "neighbour")
        detail = f"rc={rc}\n{stdout}\n{stderr}"
        return rc not in (0, 1) and "DEPTH_PROBE" not in stdout, detail

    _NO_GUARD_BAND = ("    if (mprotect(c->map, guard, PROT_NONE) != 0) {",
                      "    if (0 && mprotect(c->map, guard, PROT_NONE) != 0) {")

    def test_overrun_past_a_missing_check_faults_instead_of_entering_the_next_stack(self):
        # Defence in depth behind the entry check: an overrun must hit the guard below the
        # probe's stack (POSIX: the PROT_NONE band; Windows: the fiber's own guard page)
        # instead of writing into the neighbouring guest stack. Before the band, POSIX
        # finished this run with rc 0 and the right v0.
        faulted, detail = self._overrun_with_neighbour({"codegen.py": self._NO_ENTRY_CHECK})
        self.assertTrue(faulted, detail)

    @unittest.skipIf(sys.platform == "win32",
                     "the band is the POSIX backend's; Windows fibers end in a kernel guard page")
    def test_guard_band_mutant_overruns_silently_into_the_neighbouring_stack(self):
        faulted, detail = self._overrun_with_neighbour(
            {"codegen.py": self._NO_ENTRY_CHECK, "sr_coro.c": self._NO_GUARD_BAND})
        if faulted:
            # The mutant can only show the silent overrun when the neighbour's mapping lies
            # directly below the probe's, as top-down mmap places it on Linux.
            self.skipTest("the neighbouring stack is not adjacent on this host: " + detail)
        print("G1-no-guard-band: MUTANT_EXECUTED_AND_SEMANTIC_TEST_FAILED")

    def _probe_with_module(self, words, module_words, *, codegen_args=()):
        """One run with `module_words` translated as a second module at _MODULE_BASE."""
        with scratch_dir.TemporaryDirectory(prefix="linked_branch_module_") as tmp:
            module = Path(tmp) / "module.elf"
            _write_minimal_elf(module, module_words, base=_MODULE_BASE)
            generated, results = self._probe(
                words, (0,),
                codegen_args=(f"--extra-elf={module}@0x{_MODULE_BASE:08x}", *codegen_args),
            )
        return generated, results[0]

    def test_linked_branch_out_of_the_body_resumes_the_caller(self):
        # #968: the taken `bal` is a CALL, so the leaf's `jr $ra` returns to the link and
        # the caller adds 10 there before unwinding its own frame. As a TAIL the leaf's
        # return left the caller's body and the continuation never ran (v0 = 101).
        generated, (rc, output, fields) = self._probe_with_module(
            *_linked_branch_into_module(_BAL))
        self.assertIn("void f_00002000(CpuState *s) {", generated)
        self.assertNotIn("L_00002000", generated, "the leaf must be outside the caller's body")
        self.assertEqual(rc, 0, output)
        self.assertEqual(fields.get("v0"), 111, f"linked branch lost its continuation\n{output}")
        self.assertEqual(fields.get("a0"), 0x1020, output)
        self.assertEqual(fields.get("v1"), _PROBE_SP, output)

    def test_not_taken_linked_branch_out_of_the_body_falls_through(self):
        # `bltzal $zero` is never taken: it still links, its slot still runs once, and the
        # caller falls through to the same continuation without calling the leaf.
        _generated, (rc, output, fields) = self._probe_with_module(
            *_linked_branch_into_module(_BLTZAL))
        self.assertEqual(rc, 0, output)
        self.assertEqual(fields.get("v0"), 110, output)
        self.assertEqual(fields.get("a0"), 0x1020, output)
        self.assertEqual(fields.get("v1"), _PROBE_SP, output)

    def test_linked_branch_through_dispatch_resumes_the_native_caller(self):
        # The target is neither a label of the module's body nor a registered body: the
        # `bal` reaches the omitted primary leaf through dispatch_call, the interpreter
        # stops at the link (the resume boundary), and the native caller continues there.
        primary, module = _linked_branch_into_interpreted_body()
        generated, (rc, output, fields) = self._probe_with_module(
            primary, module, codegen_args=("--omit-aot=0x00001010",))
        self.assertNotIn("void f_00001010(CpuState *s) {", generated,
                         "the leaf must be interpreted for this cell to mean anything")
        self.assertIn("void f_00002000(CpuState *s) {", generated)
        self.assertEqual(rc, 0, output)
        self.assertEqual(fields.get("v0"), 111, f"linked branch lost its continuation\n{output}")
        self.assertEqual(fields.get("a0"), 0x2010, output)
        self.assertEqual(fields.get("v1"), _PROBE_SP, output)


if __name__ == "__main__":
    unittest.main()
