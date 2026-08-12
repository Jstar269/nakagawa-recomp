# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Guard the VFPU trace diff against float/int comparison regressions (issue #6).

``sr_end_impl`` must compare the ``s_vi`` snapshot against the raw 32-bit
register bits (``s->vi``, the integer view of the ``s->v`` float union) and
print those bits. Comparing or printing ``s->v[i]`` converts through float and
passes a double to ``%x`` — undefined varargs behavior that corrupts the trace
evidence. The portable-core compile gate enforces this class of bug with
``-Werror=format``.
"""

import re
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parent.parent
RECOMP_C = (ROOT / "src" / "rt" / "recomp.c").read_text(encoding="utf-8")
MAKEFILE = (ROOT / "Makefile").read_text(encoding="utf-8")


def sr_end_impl_body():
    match = re.search(r"void sr_end_impl\([^)]*\)\s*\{", RECOMP_C)
    if not match:
        raise AssertionError("sr_end_impl not found in src/rt/recomp.c")
    start = match.end() - 1
    depth = 0
    for pos in range(start, len(RECOMP_C)):
        char = RECOMP_C[pos]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return RECOMP_C[start : pos + 1]
    raise AssertionError("sr_end_impl has no closing brace")


class VfpuTraceBitsTests(unittest.TestCase):
    def test_vfpu_diff_compares_raw_bits(self):
        body = sr_end_impl_body()
        self.assertIn("s_vi[i] != s->vi[i]", body)

    def test_vfpu_diff_never_passes_float_to_hex_format(self):
        body = sr_end_impl_body()
        self.assertNotIn(
            "s->v[i]",
            body,
            "sr_end_impl must use the uint32 view s->vi, not the float view s->v",
        )

    def test_portable_core_gate_promotes_format_warnings(self):
        match = re.search(r"^PORTABLE_CORE_CFLAGS\s*\?=\s*(.+)$", MAKEFILE, re.MULTILINE)
        self.assertIsNotNone(match, "PORTABLE_CORE_CFLAGS not found in Makefile")
        assert match is not None
        self.assertIn("-Werror=format", match.group(1))


if __name__ == "__main__":
    unittest.main()
