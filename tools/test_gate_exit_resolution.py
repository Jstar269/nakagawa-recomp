# SPDX-License-Identifier: GPL-2.0-or-later

import unittest
from unittest.mock import MagicMock, patch
import os
import sys

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

if __name__ == "__main__":
    unittest.main()
