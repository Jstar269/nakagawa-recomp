# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Pin the nksnap v1 snapshot format and the snapshot runner's verdict grammar (#1842).

The executable proof is ``make snapshot-selftest``; these tests run anywhere and keep the
Python reader/writer (``tools/nksnap.py``), the C loader (``src/rt/nksnap.c``), the runner's
output (``src/rt/snapshot_runner.c``) and the runtime's CpuState ABI in step.
"""

from __future__ import annotations

import re
import struct
import unittest
from pathlib import Path

from tools import nksnap

ROOT = Path(__file__).resolve().parents[1]
RECOMP_H = (ROOT / "src" / "rt" / "recomp.h").read_text(encoding="utf-8")
NKSNAP_C = (ROOT / "src" / "rt" / "nksnap.c").read_text(encoding="utf-8")
RUNNER_C = (ROOT / "src" / "rt" / "snapshot_runner.c").read_text(encoding="utf-8")

DATA = 0x09000000


def _valid(blocks=None) -> nksnap.Snapshot:
    cpu = nksnap.cpu_state(r={4: 0x11, 29: 0x09100000}, hi=1, lo=2, f={3: 0x3F800000},
                           fcr31=0x01000000)
    if blocks is None:
        blocks = [nksnap.Block(DATA, b"\x01\x02\x03\x04"),
                  nksnap.Block(0x00010000, b"\xAA" * 8)]
    return nksnap.Snapshot(cpu, 0x08A00000, blocks)


class CpuStateAbiTests(unittest.TestCase):
    def test_abi_version_and_size_match_the_runtime(self):
        version = re.search(r"#define SR_CPUSTATE_ABI_VERSION (\d+)u", RECOMP_H)
        size = re.search(r"sizeof\(CpuState\) == (\d+)u", RECOMP_H)
        self.assertIsNotNone(version)
        self.assertIsNotNone(size)
        self.assertEqual(nksnap.ABI_VERSION, int(version.group(1)))
        self.assertEqual(nksnap.CPUSTATE_SIZE, int(size.group(1)))

    def test_field_offsets_match_the_runtime_asserts(self):
        pinned = dict(
            (name, int(value))
            for name, value in re.findall(r"offsetof\(CpuState, (\w+)\) == (\d+)u", RECOMP_H)
        )
        ours = {
            "cop0": nksnap.OFF_COP0, "next_pc": nksnap.OFF_NEXT_PC,
            "in_delay_slot": nksnap.OFF_IN_DELAY_SLOT, "flow_kind": nksnap.OFF_FLOW_KIND,
            "flow_target": nksnap.OFF_FLOW_TARGET, "llbit": nksnap.OFF_LLBIT,
        }
        self.assertEqual(set(pinned), set(ours))
        for name, offset in ours.items():
            self.assertEqual(offset, pinned[name], name)
        # The leading fields follow from r[32], hi, lo, pc, f[32], fcr31, fpcond, v[128],
        # vfpuCtrl[16] in declaration order, each four bytes.
        self.assertEqual(nksnap.OFF_HI, 32 * 4)
        self.assertEqual(nksnap.OFF_F, nksnap.OFF_PC + 4)
        self.assertEqual(nksnap.OFF_FCR31, nksnap.OFF_F + 32 * 4)
        self.assertEqual(nksnap.OFF_V, nksnap.OFF_FPCOND + 4)
        self.assertEqual(nksnap.OFF_VFPU_CTRL, nksnap.OFF_V + 128 * 4)
        self.assertEqual(nksnap.OFF_COP0, nksnap.OFF_VFPU_CTRL + 16 * 4)

    def test_cpu_state_places_registers(self):
        cpu = nksnap.cpu_state(r={31: 0xFFFFFFF0}, hi=5, lo=6, f={31: 7}, fcr31=8)
        self.assertEqual(len(cpu), nksnap.CPUSTATE_SIZE)
        self.assertEqual(nksnap.gpr(cpu, 31), 0xFFFFFFF0)
        self.assertEqual(struct.unpack_from("<II", cpu, nksnap.OFF_HI), (5, 6))
        self.assertEqual(struct.unpack_from("<I", cpu, nksnap.OFF_F + 31 * 4)[0], 7)
        self.assertEqual(struct.unpack_from("<I", cpu, nksnap.OFF_FCR31)[0], 8)
        self.assertEqual(struct.unpack_from("<II", cpu, nksnap.OFF_VFPU_CTRL), (0xE4, 0xE4))


class FormatTests(unittest.TestCase):
    def test_round_trip(self):
        snap = _valid()
        data = nksnap.pack(snap)
        self.assertEqual(data[:8], b"NKSNAP01")
        self.assertEqual(struct.unpack_from("<II", data, 8), (nksnap.ABI_VERSION, nksnap.CPUSTATE_SIZE))
        back = nksnap.parse(data)
        self.assertEqual(back.cpu, snap.cpu)
        self.assertEqual(back.entry_pc, snap.entry_pc)
        self.assertEqual(back.blocks, snap.blocks)
        self.assertEqual(nksnap.pack(back), data)

    def test_layout_is_exact(self):
        snap = _valid([nksnap.Block(DATA, b"\x10\x20")])
        data = nksnap.pack(snap)
        tail = data[16 + nksnap.CPUSTATE_SIZE :]
        self.assertEqual(tail, struct.pack("<IIII", 0x08A00000, 1, DATA, 2) + b"\x10\x20")

    def _refused(self, data: bytes) -> str:
        with self.assertRaises(nksnap.SnapshotError) as caught:
            nksnap.parse(data)
        return caught.exception.reason

    def test_every_refusal_reason(self):
        cpu = _valid().cpu
        good = [(DATA, 4, b"\x01\x02\x03\x04")]
        valid = nksnap.pack_raw(cpu, 0, good)
        cases = {
            "bad-magic": nksnap.pack_raw(cpu, 0, good, magic=b"NKSNAP00"),
            "abi-version": nksnap.pack_raw(cpu, 0, good, abi_version=2),
            "cpustate-size": nksnap.pack_raw(cpu + b"\0\0\0\0", 0, good),
            "truncated": valid[:-1],
            "block-size": nksnap.pack_raw(cpu, 0, [(DATA, 0, b"")]),
            "block-overlap": nksnap.pack_raw(cpu, 0, [(DATA + 4, 4, bytes(4)), (DATA, 8, bytes(8))]),
            "block-outside-guest": nksnap.pack_raw(cpu, 0, [(0x00000000, 4, bytes(4))]),
            "trailing-bytes": valid + b"\0",
        }
        self.assertEqual(set(cases), set(nksnap.REASONS))
        for reason, data in cases.items():
            with self.subTest(reason=reason):
                self.assertEqual(self._refused(data), reason)

    def test_truncation_at_every_length(self):
        valid = nksnap.pack(_valid())
        for length in range(len(valid)):
            with self.subTest(length=length):
                self.assertEqual(self._refused(valid[:length]), "truncated")

    def test_block_size_overflow(self):
        cpu = _valid().cpu
        data = nksnap.pack_raw(cpu, 0, [(0xFFFFFFF0, 0x20, b"")])
        self.assertEqual(self._refused(data), "block-size")

    def test_block_must_sit_inside_one_region(self):
        cpu = _valid().cpu
        for addr, size in ((0x0BFFFFFC, 8), (0x00013FFC, 8), (0x48000000, 4), (0x041FFFFF, 2)):
            with self.subTest(addr=hex(addr)):
                data = nksnap.pack_raw(cpu, 0, [(addr, size, bytes(size))])
                self.assertEqual(self._refused(data), "block-outside-guest")
        for first, length, name in nksnap.GUEST_REGIONS:
            with self.subTest(region=name):
                nksnap.parse(nksnap.pack_raw(cpu, 0, [(first + length - 4, 4, bytes(4))]))

    def test_adjacent_blocks_are_not_an_overlap(self):
        nksnap.parse(nksnap.pack(_valid([nksnap.Block(DATA, bytes(4)),
                                         nksnap.Block(DATA + 4, bytes(4))])))

    def test_writer_refuses_what_the_loader_refuses(self):
        with self.assertRaises(nksnap.SnapshotError) as caught:
            nksnap.pack(_valid([nksnap.Block(DATA, bytes(8)), nksnap.Block(DATA + 4, bytes(4))]))
        self.assertEqual(caught.exception.reason, "block-overlap")


class CLoaderParityTests(unittest.TestCase):
    """The C loader names the same reasons and models the same regions."""

    def test_reason_names(self):
        names = re.findall(r'case NKSNAP_\w+: return "([a-z-]+)";', NKSNAP_C)
        refusals = [n for n in names if n not in ("ok", "io-error", "out-of-memory")]
        self.assertEqual(tuple(refusals), nksnap.REASONS)

    def test_regions(self):
        regions = re.findall(r'\{ (0x[0-9a-fA-F]+)u, (0x[0-9a-fA-F]+)u, "(\w+)" \}', NKSNAP_C)
        self.assertEqual(
            tuple((int(a, 16), int(s, 16), n) for a, s, n in regions), nksnap.GUEST_REGIONS
        )

    def test_magic(self):
        self.assertIn('#define NKSNAP_MAGIC "NKSNAP01"',
                      (ROOT / "src" / "rt" / "nksnap.h").read_text(encoding="utf-8"))


class VerdictGrammarTests(unittest.TestCase):
    VALID = (
        "MATCH",
        "MISMATCH r11 aot=0x13577e7f ref=0x13577e6f",
        "MISMATCH r31 aot=0xfffffff0 ref=0x08a00010",
        "MISMATCH hi aot=0x00000001 ref=0x00000000",
        "MISMATCH f0 aot=0x3f800000 ref=0x40000000",
        "MISMATCH fcr31 aot=0x01000000 ref=0x00000000",
        "MISMATCH stop aot=import ref=syscall",
        "MISMATCH stop aot=return ref=import",
        "MISMATCH import_nid aot=0x7591c7db ref=0xdeadbeef",
        "MISMATCH syscall_code aot=0x00000123 ref=0x00000124",
        "MISMATCH oor_stores aot=0x00000001 ref=0x00000000",
        "MISMATCH mem[0x09000010] aot=0xf4 ref=0x00",
        "NOT_CHECKED ref-no-vfpu pc=0x08a0011c op=0x48e80000",
        "NOT_CHECKED ref-memory-fault pc=0x08a00000 op=0x8c880000 addr=0x00000000",
        "NOT_CHECKED ref-step-limit steps=3",
        "NOT_CHECKED write-outside-snapshot addr=0x09000010 side=both",
        "NOT_CHECKED aot-dispatch-error code=-3",
        "REFUSED truncated block 0 bytes at offset 1032",
        "REFUSED bad-magic",
    )
    INVALID = (
        "match",
        "MATCH extra",
        "MISMATCH r0 aot=0x0 ref=0x1",
        "MISMATCH r32 aot=0x0 ref=0x1",
        "MISMATCH f32 aot=0x0 ref=0x1",
        "MISMATCH pc aot=0x0 ref=0x1",
        "MISMATCH r1 aot=12 ref=0x1",
        "MISMATCH stop aot=0x1 ref=return",
        "MISMATCH r1 aot=return ref=0x1",
        "MISMATCH mem[0x9000010] aot=0x00 ref=0x01",
        "NOT_CHECKED",
        "NOT_CHECKED made-up-reason",
        "NOT_CHECKED ref-step-limit steps",
        "REFUSED not-a-reason",
        "PASS",
    )

    def test_valid_lines(self):
        for line in self.VALID:
            with self.subTest(line=line):
                verdict = nksnap.parse_verdict(line)
                self.assertEqual(verdict.kind, line.split()[0])

    def test_invalid_lines(self):
        for line in self.INVALID:
            with self.subTest(line=line):
                with self.assertRaises(ValueError):
                    nksnap.parse_verdict(line)

    def test_fields_and_details(self):
        mismatch = nksnap.parse_verdict("MISMATCH r11 aot=0x1 ref=0x2")
        self.assertEqual((mismatch.field, mismatch.aot, mismatch.ref), ("r11", "0x1", "0x2"))
        skipped = nksnap.parse_verdict("NOT_CHECKED ref-no-vfpu pc=0x08a0011c op=0x48e80000")
        self.assertEqual(skipped.reason, "ref-no-vfpu")
        self.assertEqual(skipped.details, {"pc": "0x08a0011c", "op": "0x48e80000"})

    def test_runner_not_checked_reasons_are_the_grammar(self):
        printed = set(re.findall(r'"NOT_CHECKED ([a-z][a-z-]*)', RUNNER_C))
        printed |= set(re.findall(r'"(ref-no-vfpu|ref-unimplemented)"', RUNNER_C))
        self.assertEqual(printed, set(nksnap.NOT_CHECKED_REASONS))

    def test_runner_mismatch_fields_parse(self):
        fields = set(re.findall(r'verdict_mismatch_hex\("([a-z_0-9]+)"', RUNNER_C))
        self.assertTrue({"syscall_pc", "syscall_code", "import_nid", "hi", "lo", "fcr31",
                         "oor_loads", "oor_stores"} <= fields)
        for name in fields:
            with self.subTest(field=name):
                nksnap.parse_verdict(f"MISMATCH {name} aot=0x00000000 ref=0x00000001")
        self.assertIn('"MISMATCH stop aot=%s ref=%s\\n"', RUNNER_C)
        self.assertIn('"r%d"', RUNNER_C)
        self.assertIn('"f%d"', RUNNER_C)
        self.assertIn('"MISMATCH mem[0x%08" PRIx32 "] aot=0x%02x ref=0x%02x\\n"', RUNNER_C)
        self.assertIn('"MATCH\\n"', RUNNER_C)

    def test_runner_stop_names_are_the_grammar(self):
        names = re.search(r"k_stop_names\[\] = \{ ([^}]*) \}", RUNNER_C)
        self.assertIsNotNone(names)
        self.assertEqual(tuple(re.findall(r'"(\w+)"', names.group(1))), nksnap.STOP_KINDS)


if __name__ == "__main__":
    unittest.main()
