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
import json
import os
import re
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import analyze
import codegen
import gen_nidnames
import import_fixtures
import import_stub_census
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




class EqualNidImportStubTest(unittest.TestCase):
    """A synthetic pair of equal-NID stubs with distinct file-backed addresses.

    Confirm the analyzer keeps valid ownership without inventing a shared callee.
    Import census and callable ownership match the input graph exactly.
    """

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.equal_nid = 0x7A000000
        self.nids = [self.equal_nid, self.equal_nid, 0x7A000002, 0x7A000003]
        self.data, self.stubs = import_fixtures.build_text_stub_run_elf(
            nids=self.nids
        )

    def test_equal_nid_stubs_retain_distinct_entries_and_ownership(self):
        path = os.path.join(self._tmp.name, "EBOOT.elf")
        with open(path, "wb") as fh:
            fh.write(self.data)

        elf = analyze.Elf(path, base=BASE)
        parsed = imports.parse_imports(elf)
        self.assertEqual(len(parsed), len(self.stubs))
        self.assertEqual(parsed[self.stubs[0]], ("SynthTextLib", self.equal_nid))
        self.assertEqual(parsed[self.stubs[1]], ("SynthTextLib", self.equal_nid))
        self.assertNotEqual(self.stubs[0], self.stubs[1])

        entries, ranges = analyze.analyze(elf)
        for stub in self.stubs:
            self.assertIn(stub, entries,
                          "equal-NID stub 0x%08x is not a function entry" % stub)
            self.assertTrue(analyze.in_ranges(stub, ranges),
                            "stub word 0x%08x is not owned" % stub)
            self.assertTrue(analyze.in_ranges(stub + 4, ranges),
                            "stub delay slot 0x%08x is not owned" % (stub + 4))

        census = import_stub_census.census_image(path, None, label="EBOOT.elf")
        self.assertEqual(census["status"], "ok")
        self.assertEqual(census["stubs"], len(self.stubs))
        self.assertEqual(len(census["missing"]), 0)
        self.assertEqual(len(census["runtime_missing"]), 0)


class ImportStubNameTest(unittest.TestCase):
    """--name-imports names each stub after the function its NID imports, and nothing else."""

    NAMED = ("sceGeListUpdateStallAddr", "sceDisplaySetFrameBuf")
    UNNAMED_NID = 0x7A000002  # no corpus entry: the stub keeps f_<address>

    @classmethod
    def setUpClass(cls):
        corpus = json.loads(gen_nidnames.CORPUS.read_text(encoding="utf-8"))
        names = gen_nidnames.nid_names(corpus)
        cls.nids = [gen_nidnames.nid_of(name) for name in cls.NAMED] + [cls.UNNAMED_NID]
        assert all(names.get(nid) == name for nid, name in zip(cls.nids, cls.NAMED, strict=False))
        assert cls.UNNAMED_NID not in names
        cls._tmp = tempfile.TemporaryDirectory()
        data, cls.stubs = import_fixtures.build_text_stub_run_elf(nids=cls.nids)
        cls.elf = os.path.join(cls._tmp.name, "stubs.elf")
        with open(cls.elf, "wb") as fh:
            fh.write(data)
        cls.runs = {}
        map_path = os.path.join(cls._tmp.name, "map.txt")
        with open(map_path, "w", encoding="utf-8") as fh:
            fh.write("game_stall_wrapper = 0x%08x;\n" % cls.stubs[0])
        for label, extra in (("plain", []), ("named", ["--name-imports"]),
                             ("mapped", ["--name-imports", "--symbol-map=" + map_path])):
            out_dir = os.path.join(cls._tmp.name, label)
            os.mkdir(out_dir)
            stderr = io.StringIO()
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
                rc = codegen.main(["codegen.py", cls.elf, os.path.join(out_dir, "out.c"),
                                   "--base=%08x" % BASE, *extra])
            assert rc == 0, stderr.getvalue()
            files = {}
            for name in sorted(os.listdir(out_dir)):
                with open(os.path.join(out_dir, name), encoding="ascii") as fh:
                    files[name] = fh.read()
            cls.runs[label] = (files, stderr.getvalue())

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def _chunks(self, label):
        files = self.runs[label][0]
        return "".join(text for name, text in files.items() if re.fullmatch(r"out_\d+\.c", name))

    def test_stubs_take_their_nid_names(self):
        chunks = self._chunks("named")
        for stub, name, nid in zip(self.stubs, self.NAMED, self.nids, strict=False):
            self.assertIn("void f_%08x_%s(CpuState *s) {  /* import: SynthTextLib nid 0x%08x */"
                          % (stub, name, nid), chunks)
            self.assertIn("sr_register(0x%08xu, f_%08x_%s);" % (stub, stub, name), chunks)
            self.assertIn("void f_%08x_%s(CpuState *s);" % (stub, name),
                          self.runs["named"][0]["out_funcs.h"])
        self.assertIn("void f_%08x(CpuState *s) {" % self.stubs[2], chunks)
        self.assertIn('    {0x%08xu, 0x00000000u, "sceGeListUpdateStallAddr"},' % self.stubs[0],
                      self.runs["named"][0]["out.c"])
        self.assertIn("import names: 2 of 3 import stubs named from the NID corpus",
                      self.runs["named"][1])

    def test_a_symbol_map_name_wins(self):
        chunks = self._chunks("mapped")
        self.assertIn("void f_%08x_game_stall_wrapper(CpuState *s) {" % self.stubs[0], chunks)
        self.assertNotIn("sceGeListUpdateStallAddr(", chunks)
        self.assertIn("void f_%08x_sceDisplaySetFrameBuf(CpuState *s) {" % self.stubs[1], chunks)
        self.assertIn("import names: 1 of 3 import stubs named from the NID corpus",
                      self.runs["mapped"][1])

    def test_names_change_symbol_spellings_and_nothing_else(self):
        plain, named = self.runs["plain"][0], self.runs["named"][0]
        self.assertEqual(sorted(plain), sorted(named))
        for name, text in named.items():
            undone = re.sub(r"\b(f_[0-9a-f]{8})_(?:%s)\b" % "|".join(self.NAMED), r"\1", text)
            if name == "out.c":
                undone = re.sub(
                    r"\n\n/\* Guest function names from --symbol-map[^\n]*\n"
                    r"static const SrGuestSymbol sr_guest_symbol_table\[\] = \{\n.*?\n\};",
                    "", undone, flags=re.S)
                undone = undone.replace(
                    "    sr_guest_symbols_register(sr_guest_symbol_table, 2u);\n", "")
            with self.subTest(file=name):
                self.assertEqual(undone, plain[name])
        self.assertNotIn("import names:", self.runs["plain"][1])
        self.assertNotIn("SrGuestSymbol", plain["out.c"])

    def test_makefile_passes_the_option_and_regenerates_when_the_corpus_changes(self):
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Makefile"),
                  encoding="utf-8") as fh:
            makefile = fh.read()
        self.assertIn("NAME_IMPORTS_ARG := --name-imports", makefile)
        self.assertIn("NAME_IMPORTS_PREREQ := tools/nid_corpus.json tools/gen_nidnames.py", makefile)
        self.assertIn("NAME_IMPORTS_PROFILE_LINE := $(NEWLINE)NAME_IMPORTS=1", makefile)
        rule = re.search(r"^\$\(BUILD_DIR\)/\$\(GAME_NAME\)_recomp\.c [^\n]*: ([^\n]*)\n\t([^\n]*)",
                         makefile, re.M)
        self.assertIn("$(NAME_IMPORTS_PREREQ)", rule.group(1).split())
        self.assertIn("$(NAME_IMPORTS_ARG)", rule.group(2).split())
        entries = re.search(r"^export NK_CODEGEN_PROFILE_ENTRIES := ([^\n]*)", makefile, re.M)
        self.assertIn("$(NAME_IMPORTS_PROFILE_LINE)", entries.group(1))


if __name__ == "__main__":
    unittest.main()
