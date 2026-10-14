# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Mutation proof for the production interpreter CALL/RETURN boundary.

Each mutant is applied only to a temporary copy of the production dispatch or
interpreter source.  The source-owned dispatch-isolation selftest must compile
cleanly and then fail on the semantic assertion; a compiler failure is reported
as a test failure rather than accepted as a kill.
"""

from __future__ import annotations

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
GUEST_INTERP = ROOT / "src" / "rt" / "guest_interp.c"
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
    generated_body_prefix: str = "",
) -> tuple[int, str, int, str]:
    """Generate and execute a real codegen entry wrapper against recomp.c."""
    assert CC is not None
    with scratch_dir.TemporaryDirectory(prefix="stack_census_pipeline_") as tmp:
        work = Path(tmp)
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
                sys.executable, str(CODEGEN_TOOL), str(elf), str(generated),
                "--base=0", "--profile=none", "--funcs-per-chunk=2000",
                "--stack-census", *codegen_args, *extra_codegen_args,
            ],
            cwd=ROOT, capture_output=True, text=True,
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
        fn(&g_probe_state);
        g_probe_ran = 1;
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
    g_probe_main = sr_coro_main();
    if (!g_probe_main) return 20;
    sr_dispatch_host_stack_remaining = sr_coro_stack_remaining;
    sr_sched_on = 1;
    atomic_store_explicit(&sr_timeslice, 1, memory_order_relaxed);
    SrCoro *fiber = sr_coro_create(probe_body, NULL, SR_GUEST_FIBER_STACK_SIZE);
    if (!fiber) return 21;
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


def _build_depth_probe(
    words: tuple[int, ...], work: Path, *, codegen_args: tuple[str, ...] = (),
) -> tuple[Path | None, str]:
    """Translate a source-owned fixture and link it into the depth-probe harness."""
    assert CC is not None
    elf = work / "fixture.elf"
    _write_minimal_elf(elf, words, base=_PROBE_BASE)
    generated = work / "probe.c"
    codegen = subprocess.run(
        [
            sys.executable, str(CODEGEN_TOOL), str(elf), str(generated),
            "--base=0", "--profile=none", "--funcs-per-chunk=2000", *codegen_args,
        ],
        cwd=ROOT, capture_output=True, text=True,
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
    exe = work / "depth_probe.exe"
    compile_result = subprocess.run(
        [
            CC, "-std=c11", "-O0", "-fno-strict-aliasing",
            "-Wall", "-Wextra", "-DSR_SDL3VK", "-D_CRT_SECURE_NO_WARNINGS",
            "-I", str(work), "-I", str(ROOT / "src" / "rt"),
            str(harness), str(generated), str(work / "probe_0.c"),
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
