# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class VfpuInterpreterGuardTests(unittest.TestCase):
    def test_vcrs_rejects_non_triple_width_before_source_reads(self):
        source = (ROOT / "src" / "rt" / "vfpu_interp.c").read_text(encoding="utf-8")
        marker = 'if (op == 0x19 && sub == 5) {  /* vcrs */'
        start = source.index(marker)
        block = source[start:source.index('    if (op == 0x19 && sub == 2)', start)]
        self.assertIn("if (n != 3) return SR_VFPU_OTHER;", block)
        self.assertLess(block.index("if (n != 3)"), block.index("sr_vread(a"))

    def test_vrot_overlap_scan_is_limited_to_active_lanes(self):
        source = (ROOT / "src" / "rt" / "vfpu_interp.c").read_text(encoding="utf-8")
        marker = 'if (idx == 29) {  /* vrot'
        start = source.index(marker)
        block = source[start:source.index('        return SR_VFPU_OTHER;', start)]
        self.assertIn("for (int i = 0; i < n; i++)", block)


if __name__ == "__main__":
    unittest.main()
