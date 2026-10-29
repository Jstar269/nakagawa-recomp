# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Static coverage: analyze.static_coverage, codegen's coverage report, the build report.

The fixture is a synthetic ELF32/MIPS executable written here: a translatable function
whose branch skips one word, and a callee the code generator cannot translate (a COP0
move with reserved bits set). The analyzer also starts a function at the skipped word, so
the unowned-word count is checked through the API with the two real entries only.
"""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import analyze  # noqa: E402
import title_codegen_plan  # noqa: E402
from test_codegen_entry_semantics import _synthetic_elf  # noqa: E402

ENTRY = 0x00010000
CALLEE = 0x00010020
UNOWNED = 0x00010014
WORDS = {
    0x00010000: 0x27BDFFF0,                          # addiu sp, sp, -16
    0x00010004: 0x0C000000 | (CALLEE >> 2),          # jal CALLEE
    0x00010008: 0x00000000,                          # nop (delay slot)
    0x0001000C: 0x10000002,                          # b 0x00010018, skipping UNOWNED
    0x00010010: 0x00000000,                          # nop (delay slot)
    UNOWNED: 0x24020001,                             # addiu v0, zero, 1: no edge reaches it
    0x00010018: 0x03E00008,                          # jr ra
    0x0001001C: 0x27BD0010,                          # addiu sp, sp, 16 (delay slot)
    CALLEE: 0x40026008,                              # mfc0 with reserved bits: Unsupported
    0x00010024: 0x03E00008,                          # jr ra
    0x00010028: 0x00000000,                          # nop
}


class StaticCoverageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory(prefix="static_coverage_")
        cls.work = Path(cls._tmp.name)
        cls.elf_path = cls.work / "cov.elf"
        cls.elf_path.write_bytes(_synthetic_elf(WORDS, ENTRY))
        cls.result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "codegen.py"), str(cls.elf_path),
             str(cls.work / "cov_recomp.c")],
            cwd=ROOT, capture_output=True, text=True,
        )
        cls.report_path = cls.work / "cov_recomp_coverage.json"

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def analysis(self):
        elf = analyze.Elf(str(self.elf_path))
        starts, ranges = analyze.analyze(elf)
        return elf, starts, ranges

    def test_codegen_writes_the_report_and_log_line(self):
        self.assertEqual(self.result.returncode, 0, self.result.stderr + self.result.stdout)
        self.assertTrue(self.report_path.is_file())
        line = next(line for line in self.result.stdout.splitlines()
                    if line.startswith("CODEGEN_COVERAGE "))
        self.assertIn("functions_interpreter=1", line)

    def test_report_counts_translated_and_interpreter_words(self):
        report = json.loads(self.report_path.read_text(encoding="ascii"))
        self.assertEqual(report["schema_version"], 1)
        (image,) = report["images"]
        self.assertEqual(image["image"], "executable")
        total = report["total"]
        # ENTRY, the analyzer's entry at UNOWNED, and CALLEE.
        self.assertEqual(total["functions"], 3)
        self.assertEqual(total["functions_translated"], 2)
        self.assertEqual(total["functions_interpreter"], 1)
        self.assertEqual(total["unowned_words"], 0)
        self.assertEqual(image["owner_conflict_words"], 2)   # the shared jr ra and its slot
        self.assertEqual(total["owned_words"],
                         total["translated_words"] + total["interpreter_words"]
                         + total["other_owned_words"])
        self.assertEqual(total["interpreter_words"], 3)   # the callee's three words
        self.assertEqual(total["translated_words"], 8)
        self.assertEqual(total["translated_share"],
                         round(total["translated_words"] / total["owned_words"], 6))
        self.assertEqual(image["owned_bytes"], image["owned_words"] * 4)

    def test_the_word_classes_add_up_to_the_executable_span(self):
        elf, starts, ranges = self.analysis()
        cov = analyze.static_coverage(elf, ranges, starts)
        self.assertEqual(cov["executable_words"],
                         cov["owned_words"] + cov["unowned_words"] + cov["padding_words"])
        # Nothing was translated or interpreted: every owned word is "other".
        self.assertEqual(cov["other_owned_words"], cov["owned_words"])

    def test_a_word_no_entry_reaches_is_unowned(self):
        elf, _starts, ranges = self.analysis()
        cov = analyze.static_coverage(elf, ranges, [ENTRY, CALLEE], translated={ENTRY},
                                      interpreted={CALLEE})
        self.assertEqual(cov["unowned_words"], 1)
        self.assertEqual(cov["unowned_bytes"], 4)
        self.assertEqual(cov["translated_words"], 7)
        self.assertEqual(cov["interpreter_words"], 3)
        self.assertEqual(cov["other_owned_words"], 0)

    def test_a_word_with_one_translated_owner_counts_as_translated(self):
        elf, starts, ranges = self.analysis()
        both = analyze.static_coverage(elf, ranges, starts, translated={CALLEE},
                                       interpreted={CALLEE, ENTRY})
        self.assertEqual(both["functions_translated"], 1)
        self.assertEqual(both["functions_interpreter"], 1)   # ENTRY only: translation wins

    def test_build_report_reader(self):
        self.assertIsNone(title_codegen_plan._read_static_coverage(self.work / "missing.json"))
        total = title_codegen_plan._read_static_coverage(self.report_path)
        self.assertEqual(total["functions"], 3)
        bad = self.work / "bad_coverage.json"
        bad.write_text('{"images": []}\n', encoding="ascii")
        with self.assertRaises(title_codegen_plan.PackageRouteError):
            title_codegen_plan._read_static_coverage(bad)
        self.assertTrue(title_codegen_plan._is_aot_output("game_recomp_coverage.json", "game"))


if __name__ == "__main__":
    unittest.main()
