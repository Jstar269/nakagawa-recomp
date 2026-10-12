# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Import-table stubs outside the named code sections are function entries.

A retail EBOOT keeps its import stubs in executable PT_LOAD bytes after .text,
outside .text and .sceStub.text. Discovery reached such a stub only through a
direct call from an orphan function found in the gap-fill pass, and the late
pass dropped that callee because it filtered by the named ranges alone. Codegen
then emitted no body for the stub, and the call dispatched to the image's
`jr $ra; nop` placeholder, which returns a stale $v0 with no HLE event.

These tests use a synthetic layout with the same shape (see
tools/import_fixtures.build_text_stub_run_elf); no game bytes are involved.
"""

import contextlib
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import analyze
import codegen
import import_fixtures
import imports

BASE = 0x08804000
PT_LOAD_FLAGS_OFFSET = 52 + 24  # the phdr p_flags word of the single PT_LOAD


class ImportStubEntryTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.data, self.stubs = import_fixtures.build_text_stub_run_elf(4)

    def _write(self, data, name="textstub.elf"):
        path = os.path.join(self._tmp.name, name)
        with open(path, "wb") as fh:
            fh.write(data)
        return path

    def test_every_stub_is_an_entry_that_owns_its_words(self):
        path = self._write(self.data)
        elf = analyze.Elf(path, base=BASE)
        self.assertEqual(sorted(imports.parse_imports(elf)), sorted(self.stubs))
        entries, ranges = analyze.analyze(elf)
        for stub in self.stubs:
            self.assertIn(stub, entries,
                          "import stub 0x%08x is not a function entry" % stub)
            self.assertTrue(analyze.in_ranges(stub, ranges),
                            "stub word 0x%08x is not owned" % stub)
            self.assertTrue(analyze.in_ranges(stub + 4, ranges),
                            "stub delay slot 0x%08x is not owned" % (stub + 4))

    def test_codegen_emits_a_stub_body_for_each_stub(self):
        path = self._write(self.data)
        out = os.path.join(self._tmp.name, "out.c")
        with contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            rc = codegen.main(["codegen.py", path, out, "--base=%08x" % BASE])
        self.assertEqual(rc, 0)
        # Function bodies go to the numbered chunk files, not to out.c itself.
        text = ""
        for name in sorted(os.listdir(self._tmp.name)):
            if name.startswith("out") and name.endswith(".c"):
                with open(os.path.join(self._tmp.name, name), encoding="utf-8") as fh:
                    text += fh.read()
        for stub in self.stubs:
            self.assertRegex(
                text,
                r"void f_%08x\(CpuState \*s\) \{  /\* import: SynthTextLib nid 0x[0-9a-f]+ \*/"
                % stub,
                "codegen emitted no import-stub body for 0x%08x" % stub,
            )

    def test_stub_bytes_without_the_execute_bit_stay_fail_closed(self):
        # Clearing PF_X on the only PT_LOAD leaves the stub bytes non-executable,
        # so none of them may be promoted to an entry.
        data = bytearray(self.data)
        flags = int.from_bytes(data[PT_LOAD_FLAGS_OFFSET:PT_LOAD_FLAGS_OFFSET + 4], "little")
        data[PT_LOAD_FLAGS_OFFSET:PT_LOAD_FLAGS_OFFSET + 4] = (flags & ~1).to_bytes(4, "little")
        path = self._write(bytes(data), "nonexec.elf")
        elf = analyze.Elf(path, base=BASE)
        entries, _ranges = analyze.analyze(elf)
        for stub in self.stubs:
            self.assertNotIn(stub, entries,
                             "non-executable stub 0x%08x became an entry" % stub)


class LatePhaseRangeFilterTest(unittest.TestCase):
    """The late discovery passes keep file-backed callees, not only import stubs.

    The worklist keeps a direct-call target that lies in file-backed executable bytes
    outside the named sections. Tail promotion and gap fill tested the named ranges
    alone, so a plain function reached only in a late pass (here, a second call from
    the same orphan caller) was dropped and got no body. The fixture's late callee is
    not an import stub, so this is the late-phase predicate itself, not the stub seed.
    """

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.data, self.stubs = import_fixtures.build_text_stub_run_elf(
            4, late_callee=True)
        # The callee is allocated right after the stub placeholders.
        self.callee = self.stubs[-1] + 8

    def test_a_late_callee_in_file_backed_bytes_is_an_entry_that_owns_its_words(self):
        path = os.path.join(self._tmp.name, "latecallee.elf")
        with open(path, "wb") as fh:
            fh.write(self.data)
        elf = analyze.Elf(path, base=BASE)
        entries, ranges = analyze.analyze(elf)
        self.assertIn(self.callee, entries,
                      "late callee 0x%08x dropped by the late passes" % self.callee)
        self.assertTrue(analyze.in_ranges(self.callee, ranges),
                        "late callee word 0x%08x is not owned" % self.callee)


if __name__ == "__main__":
    unittest.main()
