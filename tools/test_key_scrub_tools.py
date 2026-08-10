# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Hermetic tests for the key-history-scrub helpers (no git history / network)."""

import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_key_scrub_spec as gen  # noqa: E402
import verify_key_scrub as vks  # noqa: E402

# A synthetic 16-byte value with a sub-16 byte (0x01) and a >=16 byte, to exercise
# the bare-int Python-list spelling as well as the padded/hex forms.
SAMPLE = bytes([0x01, 0x02, 0xab, 0xCD, 0x10, 0x0f, 0x7e, 0x80,
                0x00, 0xff, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc])


class TestEncodings(unittest.TestCase):
    def test_covers_hex_and_byte_array_forms(self):
        forms = set(vks.encodings(SAMPLE))
        self.assertIn(SAMPLE.hex(), forms)                     # lowercase hex
        self.assertIn(SAMPLE.hex().upper(), forms)             # uppercase hex
        self.assertIn(",".join(f"0x{b:02x}" for b in SAMPLE), forms)   # C no-space
        self.assertIn(", ".join(f"0x{b:02x}" for b in SAMPLE), forms)  # spaced
        # The irregular Python bytes([...]) spelling: bare decimal for values < 16.
        bare = ", ".join(str(b) if b < 16 else f"0x{b:02x}" for b in SAMPLE)
        self.assertIn(bare, forms)
        self.assertIn("1", bare.split(", "))   # 0x01 rendered as bare "1"

    def test_key_values_parses_name_equals_hex(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "keys.txt"
            p.write_text(
                "# comment\nfoo = " + SAMPLE.hex() + "\nbad = notenoughhex\n",
                encoding="utf-8",
            )
            values = vks.key_values(str(p))
        self.assertEqual(values, {"foo": SAMPLE})


class TestSpecGeneration(unittest.TestCase):
    def test_spec_line_count_matches_encodings_and_uses_placeholder(self):
        values = {"a": SAMPLE, "b": bytes(range(16, 32))}
        lines = gen.build_spec(values)
        expected = len({f for raw in values.values() for f in vks.encodings(raw)})
        self.assertEqual(len(lines), expected)
        for line in lines:
            self.assertTrue(line.startswith("literal:"))
            self.assertTrue(line.endswith("==>" + gen.PLACEHOLDER))

    def test_refuses_to_write_inside_repo(self):
        repo_root = Path(gen.__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as d:
            keys = Path(d) / "keys.txt"
            keys.write_text("k = " + SAMPLE.hex() + "\n", encoding="utf-8")
            in_repo = repo_root / "should_not_be_written.txt"
            rc = gen.main(["--keys", str(keys), "--out", str(in_repo)])
        self.assertEqual(rc, 2)
        self.assertFalse(in_repo.exists())

    def test_writes_to_external_path(self):
        with tempfile.TemporaryDirectory() as d:
            keys = Path(d) / "keys.txt"
            keys.write_text("k = " + SAMPLE.hex() + "\n", encoding="utf-8")
            out = Path(d) / "spec.txt"
            rc = gen.main(["--keys", str(keys), "--out", str(out)])
            self.assertEqual(rc, 0)
            body = out.read_text(encoding="utf-8")
        self.assertIn(gen.PLACEHOLDER, body)
        self.assertIn(SAMPLE.hex(), body)


if __name__ == "__main__":
    unittest.main()
