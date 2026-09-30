# SPDX-License-Identifier: GPL-3.0-or-later

import unittest
from unittest.mock import MagicMock, patch
import os
import pathlib
import subprocess
import sys
import tempfile

# Ensure tools directory is on path
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
import codegen_gate
import microtest_gate

class TestGateExitResolution(unittest.TestCase):
    def test_shared_exit_discovery(self):
        self.assertIs(microtest_gate.find_exit_syscall_pc, codegen_gate.find_exit_syscall_pc)

    def test_shared_trace_helpers(self):
        from pathlib import Path
        import tempfile

        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / "oracle.trace"
            trace.write_bytes(
                b"# trace\r\n\r\n0 pc=0x08900000 op=0x00000000\r\n"
                b"1 pc=0x08900004 op=0x0000000C\r\n"
                b"2 pc=0x08900008 op=0x0008430C\r\n"
                b"3 pc=0x08900008 op=0x0008430C\r\n"
            )
            for gate in (codegen_gate, microtest_gate):
                with self.subTest(gate=gate.__name__):
                    self.assertEqual(gate.first_syscall_step(trace, 0x08900008), 2)
                    self.assertIsNone(gate.first_syscall_step(trace, 0x08900004))
                    self.assertIsNone(gate.first_syscall_step(trace, 0x0890000C))
            for truncate in (codegen_gate.truncate, microtest_gate.write_truncated):
                with self.subTest(truncate=truncate.__name__):
                    output = Path(directory) / "truncated.trace"
                    truncate(trace, output, 2)
                    self.assertEqual(
                        output.read_bytes(),
                        b"# trace\n0 pc=0x08900000 op=0x00000000\n"
                        b"1 pc=0x08900004 op=0x0000000C\n",
                    )
            self.assertEqual(
                microtest_gate.first_syscall_step(oracle_path=trace, exit_pc=0x08900008), 2
            )
            microtest_gate.write_truncated(oracle_path=trace, out_path=output, count=0)
            self.assertEqual(output.read_bytes(), b"# trace\n")

    @patch("analyze.Elf")
    def test_missing_exit_stub(self, MockElf):
        elf = MagicMock()
        elf.sec.side_effect = lambda name: {
            ".symtab": {"off": 0, "size": 32, "entsz": 16},
            ".strtab": {"off": 32, "size": 64}
        }.get(name)

        import struct
        sym1 = struct.pack("<IIIBBH", 0, 0x08900000, 4, 0, 0, 1)
        sym2 = struct.pack("<IIIBBH", 8, 0x08900004, 4, 0, 0, 1)
        strtab = b"main\x00_start\x00"
        elf.data = sym1 + sym2 + strtab

        MockElf.return_value = elf

        with self.assertRaises(ValueError) as ctx:
            codegen_gate.find_exit_syscall_pc("dummy.elf")
        self.assertIn("missing exit_stub", str(ctx.exception))

    @patch("analyze.Elf")
    def test_wrong_syscall_code(self, MockElf):
        elf = MagicMock()
        elf.sec.side_effect = lambda name: {
            ".symtab": {"off": 0, "size": 16, "entsz": 16},
            ".strtab": {"off": 16, "size": 64}
        }.get(name)

        import struct
        sym = struct.pack("<IIIBBH", 0, 0x08900000, 4, 0, 0, 1)
        strtab = b"exit_stub\x00"
        elf.data = sym + strtab
        elf.read_at_vaddr.return_value = struct.pack("<I", 0x0000000C)

        MockElf.return_value = elf

        with self.assertRaises(ValueError) as ctx:
            codegen_gate.find_exit_syscall_pc("dummy.elf")
        self.assertIn("synthetic syscall 0x210c not found", str(ctx.exception))

    @patch("analyze.Elf")
    def test_correct_synthetic_syscall(self, MockElf):
        elf = MagicMock()
        elf.sec.side_effect = lambda name: {
            ".symtab": {"off": 0, "size": 16, "entsz": 16},
            ".strtab": {"off": 16, "size": 64}
        }.get(name)

        import struct
        sym = struct.pack("<IIIBBH", 0, 0x08900000, 12, 0, 0, 1)
        strtab = b"exit_stub\x00"
        elf.data = sym + strtab

        def mock_read(vaddr, size):
            if vaddr == 0x08900004:
                return struct.pack("<I", 0x0008430C) # syscall 0x210c
            return struct.pack("<I", 0x00000000) # nop

        elf.read_at_vaddr.side_effect = mock_read
        MockElf.return_value = elf

        pc = codegen_gate.find_exit_syscall_pc("dummy.elf")
        self.assertEqual(pc, 0x08900004)

    def test_first_syscall_step_fail_closed(self):
        import tempfile

        trace_data = """# trace init
0 pc=0x08900000 op=0x24020001
1 pc=0x08900004 op=0x0000000C
2 pc=0x08900100 op=0x0008430C
3 pc=0x08900104 op=0x00000000
"""
        with tempfile.NamedTemporaryFile("w+", delete=False) as tf:
            tf.write(trace_data)
            trace_path = tf.name

        try:
            step = codegen_gate.first_syscall_step(trace_path, 0x08900100)
            self.assertEqual(step, 2)

            step = codegen_gate.first_syscall_step(trace_path, 0x08900004)
            self.assertIsNone(step)

            step = codegen_gate.first_syscall_step(trace_path, 0x08900200)
            self.assertIsNone(step)
        finally:
            os.unlink(trace_path)

class TestStrictTraceContract(unittest.TestCase):
    """Synthetic coverage for the opt-in strict v2 trace contract (issue #312).

    The fixtures are source-owned text written by these tests: no retail bytes,
    device captures, or private provenance.
    """

    TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tracediff.py")
    ZERO_SHA = "0123456789abcdef" * 4
    SOURCE_COMMIT = "0123456789abcdef0123456789abcdef01234567"

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = pathlib.Path(self._tmp.name)

    def run_tool(self, *paths, strict=True):
        mode = ["--strict-hardware"] if strict else []
        result = subprocess.run(
            [sys.executable, "-I", self.TOOL, *mode, *(str(path) for path in paths)],
            cwd=os.path.dirname(self.TOOL),
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=60,
        )
        return result.returncode, result.stdout + result.stderr

    def header(self, source="PSP_HARDWARE", **overrides):
        fields = {
            "source_tier": source,
            "fixture_id": "branch_delay_v1",
            "cell_id": "case_0001",
            "binary_sha256": self.ZERO_SHA,
            "source_commit": self.SOURCE_COMMIT,
            "model": "PSP-3000",
            "firmware": "6.61",
            "start_pc": "0x08900100",
            "steps": "3",
            "complete": "1",
        }
        fields.update(overrides)
        return "# psp-recomp trace v2 " + " ".join(
            f"{key}={value}" for key, value in fields.items()
        )

    def write_lines(self, name, lines):
        path = self.dir / name
        path.write_bytes(("\n".join(lines) + "\n").encode("utf-8"))
        return path

    def trace(self, name, source="PSP_HARDWARE", record_lines=None, **header):
        records = record_lines or [
            "0 pc=0x08900100 op=0x24080001 r8=0x00000001",
            "1 pc=0x08900104 op=0x25080001 r8=0x00000002 m32[0x09ffff00]=0x00000002",
            "2 pc=0x08900108 op=0x0000000c r2=0x00000000",
        ]
        return self.write_lines(name, [self.header(source, **header), *records])

    def test_matching_hardware_and_local_cosim_streams_pass(self):
        hardware = self.trace("hardware.trace")
        local = self.trace("local.trace", source="LOCAL_COSIM")
        rc, output = self.run_tool(hardware, local)
        self.assertEqual(rc, 0, output)
        self.assertIn("strict hardware traces identical", output)

    def test_identity_mismatch_is_rejected_before_record_comparison(self):
        hardware = self.trace("hardware.trace")
        for field, value in (
            ("fixture_id", "other_fixture"),
            ("cell_id", "case_0002"),
            ("binary_sha256", "abcdef0123456789" * 4),
            ("source_commit", "fedcba9876543210fedcba9876543210fedcba98"),
            ("model", "PSP-2000"),
            ("firmware", "6.60"),
            ("start_pc", "0x08900200"),
            ("steps", "2"),
        ):
            if field == "steps":
                other = self.trace(
                    f"{field}.trace",
                    record_lines=[
                        "0 pc=0x08900100 op=0x24080001 r8=0x00000001",
                        "1 pc=0x08900104 op=0x25080001 r8=0x00000002 m32[0x09ffff00]=0x00000002",
                    ],
                    **{field: value},
                )
            elif field == "start_pc":
                other = self.trace(
                    f"{field}.trace",
                    record_lines=[
                        "0 pc=0x08900200 op=0x24080001 r8=0x00000001",
                        "1 pc=0x08900104 op=0x25080001 r8=0x00000002 m32[0x09ffff00]=0x00000002",
                        "2 pc=0x08900108 op=0x0000000c r2=0x00000000",
                    ],
                    **{field: value},
                )
            else:
                other = self.trace(f"{field}.trace", **{field: value})
            with self.subTest(field=field):
                rc, output = self.run_tool(hardware, other)
                self.assertEqual(rc, 2, output)
                self.assertIn(f"identity mismatch for {field}", output)

    def test_non_hardware_source_tier_cannot_pass_hardware_gate(self):
        local_a = self.trace("local-a.trace", source="LOCAL_COSIM")
        local_b = self.trace("local-b.trace", source="LOCAL_COSIM")
        rc, output = self.run_tool(local_a, local_b)
        self.assertEqual(rc, 2, output)
        self.assertIn("requires at least one source_tier=PSP_HARDWARE", output)

    def test_ppsspp_source_tier_is_explicitly_ineligible(self):
        hardware = self.trace("hardware.trace")
        ppsspp = self.trace("ppsspp.trace", source="PPSSPP_CORROBORATIVE")
        rc, output = self.run_tool(hardware, ppsspp)
        self.assertEqual(rc, 2, output)
        self.assertIn("PPSSPP_CORROBORATIVE", output)

    def test_unknown_and_malformed_metadata_are_rejected(self):
        valid = self.trace("valid.trace")
        unknown = self.trace("unknown.trace", unexpected="value")
        rc, output = self.run_tool(valid, unknown)
        self.assertEqual(rc, 2, output)
        self.assertIn("unknown v2 metadata field", output)

        malformed = self.trace("malformed.trace", binary_sha256="not-a-sha")
        rc, output = self.run_tool(valid, malformed)
        self.assertEqual(rc, 2, output)
        self.assertIn("binary_sha256 must be lowercase SHA-256", output)

        incomplete = self.trace("incomplete.trace", complete="0")
        rc, output = self.run_tool(valid, incomplete)
        self.assertEqual(rc, 2, output)
        self.assertIn("complete must be 1", output)

        unknown_source = self.trace("unknown-source.trace", source="PSP_VITA")
        rc, output = self.run_tool(valid, unknown_source)
        self.assertEqual(rc, 2, output)
        self.assertIn("unsupported source_tier", output)

        zero_provenance = self.trace("zero-provenance.trace", binary_sha256="0" * 64)
        rc, output = self.run_tool(valid, zero_provenance)
        self.assertEqual(rc, 2, output)
        self.assertIn("all-zero placeholder", output)

        missing = self.dir / "missing.trace"
        fields = self.header().split()
        missing.write_bytes(
            ("\n".join([" ".join(fields[:-1]), "0 pc=0x08900100 op=0x24080001"]) + "\n").encode(
                "utf-8"
            )
        )
        rc, output = self.run_tool(valid, missing)
        self.assertEqual(rc, 2, output)
        self.assertIn("missing v2 metadata field", output)

    def test_unusable_step_metadata_fails_closed_without_a_traceback(self):
        valid = self.trace("valid.trace")
        for name, value in (
            ("oversized", "9" * 5000),
            ("negative", "-1"),
            ("signed", "+1"),
            ("exponent", "1e3"),
            ("hexadecimal", "0x3"),
            ("zero", "0"),
            ("over-bound", "1000001"),
        ):
            with self.subTest(steps=name):
                other = self.trace(f"steps-{name}.trace", steps=value)
                rc, output = self.run_tool(valid, other)
                self.assertEqual(rc, 2, (name, output))
                self.assertIn("REJECTED", output)
                self.assertNotIn("Traceback", output)
                if name in ("oversized",):
                    self.assertIn("digit", output)
                elif name in ("zero", "over-bound"):
                    self.assertIn("steps must be between 1 and 1000000", output)
                else:
                    self.assertIn("steps must be a decimal integer", output)

        record = "0 pc=0x08900100 op=0x24080001 r8=0x00000001"
        oversize_index = self.write_lines(
            "step-index.trace", [self.header(steps="1"), "9" * 5000 + " pc=0x08900100 op=0x24080001"]
        )
        single = self.write_lines("single.trace", [self.header(steps="1"), record])
        rc, output = self.run_tool(single, oversize_index)
        self.assertEqual(rc, 2, output)
        self.assertIn("step index", output)
        self.assertNotIn("Traceback", output)

    def test_non_utf8_trace_is_rejected_without_a_traceback(self):
        valid = self.trace("valid.trace")
        record = "0 pc=0x08900100 op=0x24080001 r8=0x00000001"
        binary = self.dir / "binary.trace"
        binary.write_bytes(
            (self.header(steps="1") + "\n" + record + "\n").encode("utf-8") + b"\xff\xfe\n"
        )
        rc, output = self.run_tool(valid, binary)
        self.assertEqual(rc, 2, output)
        self.assertIn("not valid UTF-8 text", output)
        self.assertNotIn("Traceback", output)

    def test_truncated_and_over_budget_streams_are_rejected(self):
        truncated = self.trace(
            "truncated.trace",
            record_lines=[
                "0 pc=0x08900100 op=0x24080001 r8=0x00000001",
                "1 pc=0x08900104 op=0x25080001 r8=0x00000002",
            ],
        )
        valid = self.trace("valid.trace")
        rc, output = self.run_tool(valid, truncated)
        self.assertEqual(rc, 2, output)
        self.assertIn("stream is truncated", output)

        over_budget = self.trace("over-budget.trace", steps="2")
        rc, output = self.run_tool(valid, over_budget)
        self.assertEqual(rc, 2, output)
        self.assertIn("exceeds its step budget", output)

        wrong_start = self.trace("wrong-start.trace", record_lines=[
            "0 pc=0x08900200 op=0x24080001 r8=0x00000001",
            "1 pc=0x08900104 op=0x25080001 r8=0x00000002 m32[0x09ffff00]=0x00000002",
            "2 pc=0x08900108 op=0x0000000c r2=0x00000000",
        ])
        rc, output = self.run_tool(valid, wrong_start)
        self.assertEqual(rc, 2, output)
        self.assertIn("does not match first record PC", output)

    def test_first_divergence_preserves_register_and_memory_context(self):
        hardware = self.trace("hardware.trace")
        divergent = self.trace("divergent.trace", record_lines=[
            "0 pc=0x08900100 op=0x24080001 r8=0x00000001",
            "1 pc=0x08900104 op=0x25080001 r8=0x00000003 m32[0x09ffff00]=0x00000003",
            "2 pc=0x08900108 op=0x0000000c r2=0x00000000",
        ], source="LOCAL_COSIM")
        rc, output = self.run_tool(hardware, divergent)
        self.assertEqual(rc, 1, output)
        self.assertIn("DIVERGENCE at step 1, pc 0x08900104", output)
        self.assertIn("r8=0x00000002", output)
        self.assertIn("r8=0x00000003", output)
        self.assertIn("m32[0x09ffff00]=0x00000002", output)
        self.assertIn("m32[0x09ffff00]=0x00000003", output)

    def test_first_divergence_reports_pc_and_opcode_differences(self):
        hardware = self.trace("hardware.trace")
        for name, changed, expected in (
            ("pc.trace",
             "1 pc=0x0890010c op=0x25080001 r8=0x00000002 m32[0x09ffff00]=0x00000002",
             "pc 0x08900104 vs 0x0890010c"),
            ("op.trace",
             "1 pc=0x08900104 op=0x25080002 r8=0x00000002 m32[0x09ffff00]=0x00000002",
             "op 0x25080001 vs 0x25080002"),
        ):
            with self.subTest(field=name):
                other = self.trace(name, record_lines=[
                    "0 pc=0x08900100 op=0x24080001 r8=0x00000001",
                    changed,
                    "2 pc=0x08900108 op=0x0000000c r2=0x00000000",
                ], source="LOCAL_COSIM")
                rc, output = self.run_tool(hardware, other)
                self.assertEqual(rc, 1, output)
                self.assertIn("DIVERGENCE at step 1, pc 0x08900104", output)
                self.assertIn(expected, output)
                self.assertNotIn("writes differ", output)

    def test_stream_level_rejections_omit_the_line_component(self):
        valid = self.trace("valid.trace")
        no_header = self.write_lines("no-header.trace", [""])
        not_utf8 = self.dir / "not-utf8.trace"
        not_utf8.write_bytes(b"\xff\xfe\n")
        for path in (no_header, not_utf8):
            with self.subTest(path=path.name):
                rc, output = self.run_tool(valid, path)
                self.assertEqual(rc, 2, output)
                self.assertIn(f"REJECTED: {path}: ", output)

    def test_duplicate_identity_header_is_rejected(self):
        header = self.header(steps="1")
        conflicting = self.header(model="PSP-2000", steps="1")
        record = "0 pc=0x08900100 op=0x24080001 r8=0x00000001"
        valid = self.write_lines("valid.trace", [header, record])
        rc, output = self.run_tool(valid, valid)
        self.assertEqual(rc, 0, output)

        for name, lines in (
            ("before-records.trace", [header, conflicting, record]),
            ("after-records.trace", [header, record, conflicting]),
            ("identical.trace", [header, record, header]),
        ):
            with self.subTest(duplicate=name):
                other = self.write_lines(name, lines)
                rc, output = self.run_tool(valid, other)
                self.assertEqual(rc, 2, (name, output))
                self.assertIn("duplicate trace identity header", output)

        noted = self.write_lines("comment.trace", [header, "# runner note", record])
        rc, output = self.run_tool(valid, noted)
        self.assertEqual(rc, 0, output)

        v1 = self.write_lines("v1.trace", [
            "# psp-recomp trace v1 target=fixture oracle=interp start_pc=0x08900100 steps=1",
            record,
            "# psp-recomp trace v1 target=other oracle=recomp start_pc=0x08900100 steps=1",
        ])
        rc, output = self.run_tool(v1, v1, strict=False)
        self.assertEqual(rc, 0, output)
        self.assertIn("traces identical, 1 steps", output)

    def test_v1_default_route_remains_unchanged(self):
        body = (
            "# psp-recomp trace v1 target=fixture oracle=interp "
            "start_pc=0x08900100 steps=1\n0 pc=0x08900100 op=0x00000000\n"
        )
        path_a = self.dir / "v1-a.trace"
        path_b = self.dir / "v1-b.trace"
        for path in (path_a, path_b):
            path.write_bytes(body.encode("utf-8"))
        rc, output = self.run_tool(path_a, path_b, strict=False)
        self.assertEqual(rc, 0, output)
        self.assertIn("traces identical, 1 steps", output)

        rc, output = self.run_tool(path_a, path_b)
        self.assertEqual(rc, 2, output)
        self.assertIn("expected '# psp-recomp trace v2'", output)

    def test_strict_route_reports_usage_instead_of_opening_the_flag(self):
        valid = self.trace("valid.trace")
        for arguments in ([valid], [valid, valid, valid], []):
            with self.subTest(arguments=len(arguments)):
                result = subprocess.run(
                    [sys.executable, "-I", self.TOOL, "--strict-hardware", *arguments],
                    cwd=os.path.dirname(self.TOOL),
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    timeout=60,
                )
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn("usage: tracediff.py --strict-hardware", result.stderr)
                self.assertNotIn("Traceback", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
