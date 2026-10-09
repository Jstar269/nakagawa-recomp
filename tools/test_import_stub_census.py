# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Import-stub census: every import-table stub of a synthetic image reaches the HLE.

The census (tools/import_stub_census.py) lists each import-table slot and checks that
the analyzer owns it as an entry and that codegen emits a body for it. These tests run
it over synthetic images only (tools/import_fixtures); no game bytes are involved.

The regression guard is the one the umd-io triage found: an import stub in executable
bytes after .text was dropped by the late discovery passes. Disabling the seed that
keeps such stubs (analyze._import_stub_is_file_executable) must make the census name
every stub as dropped by a late pass, so a regression can never drop stubs silently.
"""

import os
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import analyze  # noqa: E402
import import_fixtures  # noqa: E402
import imports  # noqa: E402
import import_stub_census as census  # noqa: E402

BASE = import_fixtures.BASE_VADDR
PT_LOAD_FLAGS_OFFSET = 52 + 24  # the phdr p_flags word of the single PT_LOAD


class CensusOverSyntheticImages(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.data, self.stubs = import_fixtures.build_text_stub_run_elf(4)

    def _write(self, data, name="EBOOT.elf"):
        path = os.path.join(self._tmp.name, name)
        with open(path, "wb") as fh:
            fh.write(data)
        return path

    def _census(self, data, name="EBOOT.elf"):
        return census.census_image(self._write(data, name), BASE, label=name)

    def test_every_stub_of_the_text_stub_run_reaches_an_entry_and_a_body(self):
        rec = self._census(self.data)
        self.assertEqual(rec["status"], "ok", rec)
        self.assertEqual(rec["stubs"], len(self.stubs))
        self.assertEqual(rec["missing"], [], rec["missing"])
        self.assertEqual(rec["runtime_missing"], [], "an EBOOT is not a runtime-placed module")

    def test_every_slot_has_a_handler_or_the_named_trap(self):
        rec = self._census(self.data)
        self.assertEqual(rec["handled"] + rec["trapped"], len(self.stubs))
        path = self._write(self.data)
        layout, _findings, _variables = imports.function_import_model(analyze.Elf(path, base=BASE))
        handled = frozenset(nid for _library, nid in layout.values())
        with_handlers = census.census_image(path, BASE, label="EBOOT.elf", handled=handled)
        self.assertEqual(with_handlers["handled"], len(self.stubs))
        self.assertEqual(with_handlers["trapped"], 0)

    def test_a_stub_the_late_passes_drop_is_named_as_dropped_by_a_late_pass(self):
        # Inject the umd-io regression: the analyzer reaches each stub by a direct call
        # (its report lists the call) but returns no entry for it.
        real_analyze = analyze.analyze

        def dropping_late_passes(elf, *args, report=None, **kwargs):
            functions, owned = real_analyze(elf, *args, report=report, **kwargs)
            return functions - set(self.stubs), owned

        with mock.patch.object(analyze, "analyze", side_effect=dropping_late_passes):
            rec = self._census(self.data)
        self.assertEqual(sorted({m["reason"] for m in rec["missing"]}), [census.REASON_LATE],
                         rec["missing"])
        self.assertEqual(sorted(m["addr"] for m in rec["missing"]), sorted(self.stubs))

    def test_the_late_pass_keeps_stubs_even_without_the_seed(self):
        # With the callee rule of the late passes, a file-backed stub reached by a direct
        # call stays an entry that owns its words, so the census is clean without the seed.
        with mock.patch.object(analyze, "_import_stub_is_file_executable",
                               side_effect=lambda *_: False):
            rec = self._census(self.data)
        self.assertEqual(rec["missing"], [], rec["missing"])

    def test_a_stub_outside_every_executable_range_is_named_as_such(self):
        data = bytearray(self.data)
        flags = int.from_bytes(data[PT_LOAD_FLAGS_OFFSET:PT_LOAD_FLAGS_OFFSET + 4], "little")
        data[PT_LOAD_FLAGS_OFFSET:PT_LOAD_FLAGS_OFFSET + 4] = (flags & ~1).to_bytes(4, "little")
        rec = self._census(bytes(data))
        self.assertEqual(sorted({m["reason"] for m in rec["missing"]}), [census.REASON_OUTSIDE])
        self.assertEqual(len(rec["missing"]), len(self.stubs))

    def test_a_variable_import_image_names_each_function_stub(self):
        blob = bytearray(import_fixtures.build_interleaved_import_elf(
            [("SynthAlpha", 0, 1), ("SynthVariables", 1, 0)], [0x11000001]))
        # The variable-only entry's variable-stub pointer must be non-null (as in the
        # analyzer's own boundary test); the analyzer then refuses the image by name.
        rec = self._census(_point_variable_table_at_modinfo(bytes(blob)))
        self.assertEqual(rec["status"], "refused", rec)
        self.assertEqual(rec["refusal"], census.VARIABLE_IMPORTS_CODE)
        self.assertEqual(len(rec["missing"]), 1, rec["missing"])
        self.assertEqual(rec["missing"][0]["reason"], census.REASON_VARIABLE)
        self.assertEqual(rec["missing"][0]["nid"], 0x11000001)

    def test_a_malformed_import_table_is_a_named_refusal_not_a_crash(self):
        # A function window with no NID table is refused by the window walk; the census
        # names the image and its code, and the slots it cannot enumerate are one finding.
        blob = import_fixtures.build_import_elf([("SynthAlpha", [0x11000001])],
                                                corrupt="null_nid_table")
        rec = self._census(blob)
        self.assertEqual(rec["status"], "refused", rec)
        self.assertEqual(rec["refusal"], "ANALYZER_IMPORT_NID_TABLE_MISSING", rec)
        self.assertEqual([m["reason"] for m in rec["missing"]], [census.REASON_OTHER])

    def test_a_layout_model_refusal_is_recorded_not_raised(self):
        # The layout model raises its own ImportTableError class (psp_import_table) when
        # stub and NID regions do not pair 1:1. That must be a recorded refusal too.
        # Both parse paths (the census's and the analyzer's) see the same refusal.
        layout_error = imports.ImportTableError(
            "ANALYZER_IMPORT_REGIONS_MISMATCH",
            "import stub region size does not match NID region size")
        with mock.patch.object(imports, "function_import_model", side_effect=layout_error), \
                mock.patch.object(imports, "parse_imports", side_effect=layout_error):
            rec = self._census(self.data)
        self.assertEqual(rec["status"], "refused", rec)
        self.assertEqual(rec["refusal"], "ANALYZER_IMPORT_REGIONS_MISMATCH", rec)
        # The loader's own rule names every slot of the windows, so none is silent.
        self.assertEqual(sorted(m["addr"] for m in rec["missing"]), sorted(self.stubs))
        self.assertEqual({m["reason"] for m in rec["missing"]}, {census.REASON_OTHER})

    def test_split_nid_windows_are_refused_and_every_slot_is_named(self):
        # Two windows that pair at different offsets, in an image without the
        # psp-fixup-imports sections: the layout refuses it (a documented, fail-closed
        # policy; see psp_import_table). The census still names all four slots, with
        # the NID each window gives, so the decision stays visible per stub.
        blob, expected = build_split_nid_windows_elf()
        rec = self._census(blob)
        self.assertEqual(rec["status"], "refused", rec)
        self.assertEqual(rec["refusal"], "ANALYZER_IMPORT_REGIONS_MISMATCH", rec)
        self.assertEqual({m["addr"]: m["nid"] for m in rec["missing"]}, expected, rec["missing"])
        self.assertEqual({m["reason"] for m in rec["missing"]}, {census.REASON_OTHER})

    def test_an_unreadable_image_is_one_named_missing_finding(self):
        rec = census.census_image(self._write(b"not an elf at all"), BASE, label="junk.elf")
        self.assertEqual(rec["status"], "error", rec)
        self.assertEqual([m["reason"] for m in rec["missing"]], [census.REASON_OTHER])
        self.assertEqual(rec["reasons"], {census.REASON_OTHER: 1})

    def test_a_guest_module_keeps_stubs_outside_its_named_sections_as_runtime_misses(self):
        rec = self._census(self.data, name="guest.prx")
        self.assertEqual(rec["missing"], [])
        self.assertEqual(sorted(m["addr"] for m in rec["runtime_missing"]), sorted(self.stubs))


def _point_variable_table_at_modinfo(blob):
    """Set the variable-only entry's vstub pointer to the module info (base-0 fixture)."""
    elf = analyze.Elf(blob, base=0)
    header = elf.read_at_vaddr(elf.sec(".rodata.sceModuleInfo")["addr"], 52)
    libstub = struct.unpack_from("<I", header, 44)[0]
    offset = import_fixtures.DATA_FILE_OFF + (libstub - BASE) + 20 * 1 + 20
    out = bytearray(blob)
    struct.pack_into("<I", out, offset, BASE)
    return bytes(out)


def build_split_nid_windows_elf():
    """A sectionless image whose two windows pair at different offsets.

    This is the shape of a retail PRX whose stub run and NID arrays do not
    match the psp-fixup-imports sections: each window pairs with its own NID array, but
    the arrays sit apart, so no single pairing region covers both. The layout model
    refuses such a table fail-closed; the census must still name all four slots.
    """
    seg = bytearray()

    def alloc(blob, align=4):
        while len(seg) % align:
            seg.append(0)
        off = len(seg)
        seg.extend(blob)
        return BASE + off

    modinfo = alloc(import_fixtures._module_info_record(0, b"SynthSplit"))
    name_a = alloc(b"SynthA\0", 1)
    name_b = alloc(b"SynthB\0", 1)
    nids_a = alloc(struct.pack("<2I", 0x11000001, 0x11000002))
    alloc(b"\0" * 12)  # unreferenced words between the two NID arrays
    nids_b = alloc(struct.pack("<2I", 0x22000001, 0x22000002))
    stubs = alloc(import_fixtures.STUB_PLACEHOLDER * 4)
    entries = (struct.pack("<IHHBBHII", name_a, 0x0101, 0x0009, 5, 0, 2, nids_a, stubs)
               + struct.pack("<IHHBBHII", name_b, 0x0101, 0x0009, 5, 0, 2, nids_b, stubs + 16))
    libstub = alloc(entries)
    struct.pack_into("<II", seg, (modinfo - BASE) + 44, libstub, libstub + len(entries))
    expected = {stubs: 0x11000001, stubs + 8: 0x11000002,
                stubs + 16: 0x22000001, stubs + 24: 0x22000002}
    return import_fixtures._elf(bytes(seg), modinfo, sectionless=True), expected


class CensusGateAndReporting(unittest.TestCase):
    def _record(self, **overrides):
        rec = {"image": "x", "status": "ok", "refusal": None, "detail": "", "stubs": 1,
               "handled": 0, "trapped": 1, "missing": [], "runtime_missing": [],
               "reasons": {}}
        rec.update(overrides)
        return rec

    def test_check_passes_a_census_with_no_missing_stub(self):
        self.assertEqual(census.check_failures([self._record()]), [])

    def test_check_fails_on_a_missing_stub_and_on_an_analyzer_fault(self):
        missing = self._record(missing=[census._missing(0x08800000, "L", 1, census.REASON_OUTSIDE)])
        fault = self._record(status="error", stubs=0)
        self.assertEqual(len(census.check_failures([missing, fault])), 2)

    def test_a_named_decision_reason_does_not_fail_the_gate(self):
        variable = self._record(missing=[census._missing(0x08800000, "L", 1, census.REASON_VARIABLE)])
        self.assertEqual(census.check_failures([variable], (census.REASON_VARIABLE,)), [])
        self.assertEqual(len(census.check_failures([variable])), 1)

    def test_handled_nids_are_the_sr_hle_registrations(self):
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, "src", "rt"))
            with open(os.path.join(root, "src", "rt", "hle_x.c"), "w", encoding="utf-8") as fh:
                fh.write('sr_hle_register(0xf3f76017, "a", h_a);\n'
                         'sr_hle_register(0x3dfaeba9u, "b", h_b);\n')
            self.assertEqual(census.handled_nids(root), frozenset({0xf3f76017, 0x3dfaeba9}))

    def test_corpus_discovery_takes_executables_and_modules_with_their_bases(self):
        with tempfile.TemporaryDirectory() as root:
            for title, names in (("TITLE-A", ["EBOOT.elf", "lib.prx", "notes.txt"]),
                                 ("TITLE-B", ["EBOOT.elf"])):
                os.makedirs(os.path.join(root, title, "decrypted"))
                for name in names:
                    open(os.path.join(root, title, "decrypted", name), "wb").close()
            found = census.discover_corpus(root)
            self.assertEqual(sorted(label for _p, label, _b in found),
                             ["TITLE-A/EBOOT.elf", "TITLE-A/lib.prx", "TITLE-B/EBOOT.elf"])
            bases = {label: base for _p, label, base in found}
            self.assertIsNone(bases["TITLE-A/EBOOT.elf"])
            self.assertEqual(bases["TITLE-A/lib.prx"], 0)

    def test_rendered_census_names_the_reason_and_the_entry(self):
        record = self._record(missing=[census._missing(0x08800010, "SynthLib", 0x11000001,
                                                       census.REASON_LATE, "detail")])
        record["reasons"] = {census.REASON_LATE: 1}
        text = census.render_markdown([record])
        self.assertIn("dropped by a late pass", text)
        self.assertIn("0x08800010", text)
        self.assertIn("0x11000001", text)


if __name__ == "__main__":
    unittest.main()
