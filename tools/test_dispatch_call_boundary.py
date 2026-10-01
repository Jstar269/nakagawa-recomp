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
import tempfile
import unittest
from pathlib import Path


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
) -> tuple[int, str, int, str]:
    """Generate and execute a real codegen entry wrapper against recomp.c."""
    assert CC is not None
    with tempfile.TemporaryDirectory(prefix="stack_census_pipeline_") as tmp:
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


def _build_and_run(mutated_recomp: str | None = None,
                   mutated_interp: str | None = None) -> tuple[int, str, int, str]:
    """Build and run the real selftest against temporary source copies."""
    assert CC is not None
    with tempfile.TemporaryDirectory(prefix="dispatch_call_boundary_mut_") as tmp:
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
                "            pc = transfer.taken ? transfer.target : pc + 8u;"
            ),
            interp_new=(
                "            (void)execute_noncontrol(s, pc + 4u, delay_opcode,\n"
                "                                     &delay_store_address, "
                "&delay_store_size, fault);\n"
                "            instruction_count += 2u;\n"
                "            if (sr_perf_enabled) sr_perf_interp_instruction();\n"
                "            if (sr_perf_enabled) sr_perf_interp_instruction();\n"
                "            pc = transfer.taken ? transfer.target : pc + 8u;"
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


if __name__ == "__main__":
    unittest.main()
