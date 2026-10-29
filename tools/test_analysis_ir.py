# SPDX-License-Identifier: GPL-3.0-or-later

"""The advisory analysis IR (#305): parity with the analyzer, round trips, annotations.

The IR is built from the CFG report that ``codegen.py --cfg-report`` writes, so
these tests run the real codegen on source-owned synthetic ELFs and check that the
IR's callables, blocks and owners match the report, that building it leaves the
report untouched, that its JSON round-trips, and that external annotations are
compared with the analyzer without ever changing what it decided.
"""

from __future__ import annotations

import copy
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import analysis_ir  # noqa: E402

BASE = 0x1000
NOP = 0x00000000
JR_RA = 0x03E00008


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def write_elf(path: Path, words) -> None:
    """A minimal ET_EXEC with one executable PT_LOAD at BASE and no sections."""
    payload_off = 52 + 32
    blob = bytearray(payload_off + len(words) * 4)
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into("<HHIIIIIHHHHHH", blob, 16, 2, 8, 1, BASE, 52, 0, 0, 52, 32, 1, 0, 0, 0)
    struct.pack_into("<8I", blob, 52, 1, payload_off, BASE, BASE, len(words) * 4, len(words) * 4, 5, 4)
    for index, word in enumerate(words):
        struct.pack_into("<I", blob, payload_off + index * 4, word & 0xFFFFFFFF)
    path.write_bytes(blob)


# f_00001000: call f_00001020, then a counted loop, then return.
# f_00001020: return. f_00001028: a computed tail call through $t9.
CALL_LOOP_WORDS = (
    0x27BDFFF0,              # 0x1000 addiu sp, sp, -16
    jal(BASE + 0x20),        # 0x1004 jal 0x1020
    NOP,                     # 0x1008
    0x24840001,              # 0x100c addiu a0, a0, 1
    0x1485FFFE,              # 0x1010 bne a0, a1, 0x100c
    NOP,                     # 0x1014
    JR_RA,                   # 0x1018
    NOP,                     # 0x101c
    JR_RA,                   # 0x1020 f_00001020
    0x24020005,              # 0x1024 addiu v0, zero, 5 (delay slot)
)


class AnalysisIrTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def _report(self, words, name="fixture"):
        elf = self.root / f"{name}.elf"
        write_elf(elf, words)
        out = self.root / name
        out.mkdir()
        report = out / "cfg.json"
        proc = subprocess.run(
            [sys.executable, str(TOOLS / "codegen.py"), str(elf), str(out / "out.c"), "--base=0",
             "--profile=none", "--funcs-per-chunk=2000", f"--cfg-report={report}"],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        return json.loads(report.read_text(encoding="utf-8"))

    def test_ir_ownership_matches_the_report_and_leaves_it_untouched(self):
        report = self._report(CALL_LOOP_WORDS)
        before = copy.deepcopy(report)
        ir = analysis_ir.build_ir(report)
        self.assertEqual(report, before, "building the IR changed the CFG report")
        self.assertEqual(analysis_ir.ownership_parity(ir, report), [])
        self.assertEqual([(f["entry"], f["kind"]) for f in ir["functions"]],
                         [(0x1000, "callable"), (0x1020, "callable")])
        starts = [b["start"] for b in ir["blocks"]]
        self.assertEqual(starts, [0x1000, 0x100C, 0x1018, 0x1020])
        caller = ir["functions"][0]
        self.assertEqual(caller["calls"], [{"site": 0x1004, "target": 0x1020}])
        self.assertEqual(ir["functions"][1]["callers"], [{"caller": 0x1000, "site": 0x1004, "kind": "call"}])
        loop = next(b for b in ir["blocks"] if b["start"] == 0x100C)
        self.assertIn({"source": 0x1010, "target": 0x100C, "kind": "branch", "detail": "conditional-branch"},
                      loop["successors"])
        ret = next(b for b in ir["blocks"] if b["start"] == 0x1018)
        self.assertEqual(ret["successors"][0]["kind"], "return")
        slot = next(i for i in ir["instructions"] if i["address"] == 0x1008)
        self.assertEqual(slot["delay_slot_of"], 0x1004)

    def test_parity_names_a_tampered_owner(self):
        report = self._report(CALL_LOOP_WORDS)
        ir = analysis_ir.build_ir(report)
        ir["instructions"][3]["owners"] = [0x1020]
        codes = {f["code"] for f in analysis_ir.ownership_parity(ir, report)}
        self.assertIn("instruction-owners", codes)
        ir = analysis_ir.build_ir(report)
        ir["functions"].pop()
        codes = {f["code"] for f in analysis_ir.ownership_parity(ir, report)}
        self.assertIn("function-missing", codes)

    def test_json_round_trip_is_stable(self):
        ir = analysis_ir.build_ir(self._report(CALL_LOOP_WORDS))
        text = analysis_ir.canonical_json(ir)
        self.assertEqual(analysis_ir.canonical_json(analysis_ir.load_ir(text)), text)
        with self.assertRaises(analysis_ir.IRError):
            analysis_ir.load_ir(json.dumps({"schema": "something-else"}))

    def test_a_report_without_codegen_ownership_is_refused(self):
        report = self._report(CALL_LOOP_WORDS)
        report.pop("emitted_ownership_map")
        with self.assertRaises(analysis_ir.IRError) as caught:
            analysis_ir.build_ir(report)
        self.assertEqual(caught.exception.code, "IR_REPORT_NO_OWNERSHIP_MAP")

    def test_register_effects(self):
        self.assertEqual(analysis_ir.gpr_effects(0x00851021),   # addu v0, a0, a1
                         {"known": True, "reads": [4, 5], "writes": [2], "memory": None})
        self.assertEqual(analysis_ir.gpr_effects(0x8C880004)["memory"], {"access": "load", "width": 4})
        self.assertEqual(analysis_ir.gpr_effects(0xA0880004)["memory"], {"access": "store", "width": 1})
        self.assertEqual(analysis_ir.gpr_effects(jal(0x2000))["writes"], [31])
        self.assertEqual(analysis_ir.gpr_effects(0x0085100A)["reads"], [2, 4, 5])  # movz keeps v0
        self.assertEqual(analysis_ir.gpr_effects(0x00000021)["writes"], [])        # writes r0
        self.assertFalse(analysis_ir.gpr_effects(0x70000000)["known"])            # SPECIAL2 halt

    def _annotations(self, rows):
        return analysis_ir.load_annotations({"schema": analysis_ir.ANNOTATION_SCHEMA, "annotations": rows})

    def test_annotations_never_change_the_ir(self):
        ir = analysis_ir.build_ir(self._report(CALL_LOOP_WORDS))
        before = analysis_ir.canonical_json(ir)
        rows = self._annotations([
            {"address": 0x1000, "kind": "function-start", "source": "ghidra", "evidence_class": "decompiler",
             "confidence": "high"},
            {"address": 0x100C, "kind": "function-start", "source": "ghidra", "evidence_class": "decompiler",
             "confidence": "low"},
            {"address": 0x1020, "kind": "not-function-start", "source": "trace", "evidence_class": "runtime",
             "confidence": "medium"},
            {"address": 0x1020, "kind": "function-name", "name": "get_five", "source": "manual",
             "evidence_class": "manual", "confidence": "medium"},
        ])
        result = analysis_ir.apply_annotations(ir, rows)
        self.assertEqual(analysis_ir.canonical_json(ir), before)
        self.assertEqual(result["counts"], {"agreements": 1, "advisories": 1, "disagreements": 2,
                                            "measured_disagreements": 1})
        interior = next(d for d in result["disagreements"] if d["annotation"]["address"] == 0x100C)
        self.assertEqual(interior["analyzer"], "interior of 0x00001000")
        self.assertEqual([f["entry"] for f in ir["functions"]], [0x1000, 0x1020])

    def test_exported_annotations_round_trip_as_agreements(self):
        ir = analysis_ir.build_ir(self._report(CALL_LOOP_WORDS))
        exported = analysis_ir.export_annotations(ir)
        result = analysis_ir.apply_annotations(ir, analysis_ir.load_annotations(exported))
        self.assertEqual(result["counts"]["disagreements"], 0)
        self.assertEqual(result["counts"]["advisories"], 0)
        self.assertEqual(result["counts"]["agreements"], len(exported["annotations"]))

    def test_malformed_annotations_are_refused(self):
        for row in ({"address": -1, "kind": "function-start", "source": "x", "evidence_class": "manual",
                     "confidence": "low"},
                    {"address": 4, "kind": "rename", "source": "x", "evidence_class": "manual",
                     "confidence": "low"},
                    {"address": 4, "kind": "function-start", "source": "", "evidence_class": "manual",
                     "confidence": "low"},
                    {"address": 4, "kind": "function-start", "source": "x", "evidence_class": "oracle",
                     "confidence": "low"},
                    {"address": 4, "kind": "indirect-targets", "source": "x", "evidence_class": "manual",
                     "confidence": "low", "targets": []}):
            with self.assertRaises(analysis_ir.IRError):
                self._annotations([row])

    def test_explain_a_computed_tail_call(self):
        # f_00001000 ends in `jr $t9`: the analyzer leaves the target to the dispatcher.
        words = (0x3C190000, 0x37391020, 0x03200008, NOP, 0, 0, 0, 0, JR_RA, NOP)
        ir = analysis_ir.build_ir(self._report(words, "tail"))
        self.assertEqual(ir["functions"][0]["unresolved_indirect"], [0x1008])
        rows = self._annotations([{"address": 0x1008, "kind": "indirect-targets", "targets": [0x1020],
                                   "source": "manual", "evidence_class": "manual", "confidence": "high"}])
        result = analysis_ir.apply_annotations(ir, rows)
        self.assertEqual(result["counts"]["advisories"], 1)
        text = "\n".join(analysis_ir.explain(ir, 0x1008, result))
        self.assertIn("unresolved indirect transfer", text)
        self.assertIn("annotation (advisory): indirect-targets from manual", text)

    def test_command_line_build_verify_and_explain(self):
        report = self._report(CALL_LOOP_WORDS)
        report_path = self.root / "report.json"
        report_path.write_text(json.dumps(report), encoding="utf-8")
        ir_path = self.root / "ir.json"
        run = lambda *args: subprocess.run([sys.executable, str(TOOLS / "analysis_ir.py"), *args],
                                           cwd=ROOT, capture_output=True, text=True, check=False)
        built = run("build", "--cfg-report", str(report_path), "--out", str(ir_path))
        self.assertEqual(built.returncode, 0, built.stderr)
        self.assertIn("ANALYSIS_IR functions=2", built.stderr)
        verified = run("verify", "--ir", str(ir_path), "--cfg-report", str(report_path))
        self.assertEqual(verified.returncode, 0, verified.stdout)
        self.assertIn("ANALYSIS_IR_PARITY PASS", verified.stdout)
        explained = run("explain", "--ir", str(ir_path), "0x1020")
        self.assertIn("reached from 0x00001004 by call (jal)", explained.stdout)


if __name__ == "__main__":
    unittest.main()
