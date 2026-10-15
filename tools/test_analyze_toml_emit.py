# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Locks the analyze.py TOML emitter's fail-closed string contract.

emit_toml is the one place analyzer output interpolates arbitrary strings into
a host-syntax document. Guest bytes never reach it today (model fields are
host constants or round-tripped TOML), but the emitter must never silently
produce a malformed document: strings are escaped for basic strings and
control characters fail closed instead of being emitted raw.
"""

from __future__ import annotations

import sys
import tomllib
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

from analyze import _fmt_value  # noqa: E402


class TestTomlEmit(unittest.TestCase):
    def test_string_escape_contract(self):
        self.assertEqual(_fmt_value("name", 'a"b\\c'), r'"a\"b\\c"')

    def test_plain_strings_pass_through(self):
        self.assertEqual(_fmt_value("kind", "function"), '"function"')
        self.assertEqual(_fmt_value("note", "bal tail"), '"bal tail"')

    def test_non_string_values(self):
        self.assertEqual(_fmt_value("addr", 0x1234), "0x00001234")
        self.assertEqual(_fmt_value("entry", 0x8000), "0x00008000")
        self.assertEqual(_fmt_value("skip", True), "true")
        self.assertEqual(_fmt_value("size", 42), "42")

    def test_control_characters_are_rejected_fail_closed(self):
        for hostile in ("a\nb", "a\rb", "a\tb", "a\x01b", "\x00", "a\x1fb", "a\x7fb"):
            with self.assertRaisesRegex(ValueError, "control"):
                _fmt_value("note", hostile)

    def test_printable_unicode_output_is_toml_parseable(self):
        value = "café π 🙂"
        rendered = _fmt_value("note", value)
        parsed = tomllib.loads(f"note = {rendered}\n")
        self.assertEqual(parsed["note"], value)


    def test_emit_toml_insertion_order_determinism(self):
        import tempfile
        from analyze import emit_toml

        fn_a = {"addr": 0x8000, "kind": "function", "z_extra": "value_z", "a_extra": "value_a"}
        fn_b = {"addr": 0x8004, "kind": "function", "a_extra": "value_a", "z_extra": "value_z"}

        model1 = {"module": "elf", "entry": 0x8000, "functions": [fn_b, fn_a]}
        model2 = {"module": "elf", "entry": 0x8000, "functions": [fn_a, fn_b]}

        with tempfile.NamedTemporaryFile(mode="w+", delete=False) as f1, tempfile.NamedTemporaryFile(mode="w+", delete=False) as f2:
            emit_toml(model1, f1.name)
            emit_toml(model2, f2.name)
            out1 = Path(f1.name).read_text(encoding="ascii")
            out2 = Path(f2.name).read_text(encoding="ascii")

        self.assertEqual(out1, out2)

    def test_cfg_edge_serializer_order_determinism(self):
        from analyze import _canonical_cfg_report_reference

        class FakeImage:
            def __init__(self):
                self.sections = []
                self.reloc = None
                self.entry_point = 0x1000

            def read_at_vaddr(self, addr, size):
                if addr == 0x1000:
                    return (0x0C000800).to_bytes(4, "little")  # jal 0x2000
                elif addr == 0x1004:
                    return (0x00000000).to_bytes(4, "little")  # nop
                elif addr == 0x2000:
                    return (0x03E00008).to_bytes(4, "little")  # jr ra
                return None

        img = FakeImage()
        rep1 = _canonical_cfg_report_reference(img, ranges=[(0x1000, 0x1008), (0x2000, 0x2004)], entries=[0x1000])
        rep2 = _canonical_cfg_report_reference(img, ranges=[(0x2000, 0x2004), (0x1000, 0x1008)], entries=[0x1000])
        self.assertEqual(rep1, rep2)


    def test_cfg_edge_serializer_detail_order_determinism(self):
        from analyze import CanonicalCfgState

        st1 = CanonicalCfgState()
        st1.addresses.append(0x1000)
        st1.words.append(0)
        st1.terminators.append(0)
        st1.delay_of.append(0x100000000)
        st1.delay_annulled.append(0)
        st1.owner_single.append(0x100000000)
        st1.owner_reason_masks.append(0)

        # Add two edges from source index 0 (0x1000) to target 0x2000 with same kind 'call' but different details
        # detail 3 = jal, detail 5 = linking-branch
        st1.edge_sources.append(0)
        st1.edge_targets.append(0x2000)
        st1.edge_kinds.append(2)  # call
        st1.edge_details.append(3)  # jal

        st1.edge_sources.append(0)
        st1.edge_targets.append(0x2000)
        st1.edge_kinds.append(2)  # call
        st1.edge_details.append(5)  # linking-branch

        st2 = CanonicalCfgState()
        st2.addresses.append(0x1000)
        st2.words.append(0)
        st2.terminators.append(0)
        st2.delay_of.append(0x100000000)
        st2.delay_annulled.append(0)
        st2.owner_single.append(0x100000000)
        st2.owner_reason_masks.append(0)

        # Reverse edge insertion order
        st2.edge_sources.append(0)
        st2.edge_targets.append(0x2000)
        st2.edge_kinds.append(2)  # call
        st2.edge_details.append(5)  # linking-branch

        st2.edge_sources.append(0)
        st2.edge_targets.append(0x2000)
        st2.edge_kinds.append(2)  # call
        st2.edge_details.append(3)  # jal

        rep1 = st1.to_report()
        rep2 = st2.to_report()

        self.assertEqual(rep1["call_edges"], rep2["call_edges"])
        self.assertEqual(rep1["edges"], rep2["edges"])


if __name__ == "__main__":
    unittest.main()
