# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-09-21.
# See NOTICE.md for upstream lineage and modification provenance.
#
# Synthetic fail-closed tests for tools/nidseq.py (issue #381).
# No retail assets, traces, or import tables: every fixture is synthesized here.
# The suite was first run against the pre-fix tool to record the failing-before
# behavior (zero-vs-zero agreement, prefix-as-equivalence, DIVERGE with exit 0,
# KeyError crash on a missing import table); now it pins the fail-closed contract.
import os
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(TOOLS, "nidseq.py")

STUBS = [0x08A00000, 0x08A00008, 0x08A00010, 0x08A00018]
NIDS = [0x00000111, 0x00000222, 0x00000333, 0x00000444]
FILLER_PC = 0x08800100


def run_tool(argv):
    proc = subprocess.run(
        [sys.executable, "-I", TOOL] + argv,
        capture_output=True,
        text=True,
        cwd=TOOLS,
        encoding="utf-8",
        errors="replace",
        timeout=60,
    )
    return proc.returncode, proc.stdout + proc.stderr


class TestNidseqFailClosed(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.d = self._tmp.name
        self.n = 0

    def write_imports(self, entries, name="imports.toml"):
        """entries: list of (stub, nid) pairs; an empty list writes `import = []`."""
        p = os.path.join(self.d, name)
        with open(p, "w", newline="\n") as f:
            if entries:
                for stub, nid in entries:
                    f.write("[[import]]\n")
                    f.write(f"stub = 0x{stub:08x}\n")
                    f.write('lib = "sceUtility"\n')
                    f.write(f"nid = 0x{nid:08x}\n")
            else:
                f.write("import = []\n")
        return p

    def trace(self, seq, name=None, extra_lines=()):
        """seq: list of stub pcs to hit; renders the stub's jr line plus a filler step."""
        self.n += 1
        p = os.path.join(self.d, name or f"t{self.n}.trace")
        lines = list(extra_lines)
        for pc in seq:
            lines.append(f"{len(lines)} pc=0x{pc:08x} op=0x03e00008 r2=0x1")
            lines.append(f"{len(lines)} pc=0x{FILLER_PC:08x} op=0x00000000 r2=0x0")
        with open(p, "w", newline="\n") as f:
            if lines:
                f.write("\n".join(lines) + "\n")
        return p

    def toml_with(self, entries):
        return self.write_imports([(STUBS[i], NIDS[i]) for i in entries])

    # --- verification mode, default (exact equality) -----------------------

    def test_exact_nonempty_sequence_verifies(self):
        toml = self.toml_with(range(3))
        rc, out = run_tool([toml, self.trace(STUBS[:3]), self.trace(STUBS[:3])])
        self.assertEqual(rc, 0, out)
        self.assertIn("VERIFIED: import sequences identical (3 imports)", out)

    def test_divergence_fails_nonzero(self):
        toml = self.toml_with(range(4))
        rc, out = run_tool([toml, self.trace(STUBS[:2]), self.trace([STUBS[0], STUBS[3]])])
        self.assertEqual(rc, 1, out)
        self.assertIn("DIVERGE at import 1", out)
        self.assertIn("FAILED: import sequences diverge", out)
        self.assertIn("recomp=sceUtility.0x00000222", out)
        self.assertIn("oracle=sceUtility.0x00000444", out)

    def test_zero_vs_zero_fails(self):
        toml = self.toml_with(range(2))
        rc, out = run_tool([toml, self.trace([]), self.trace([])])
        self.assertEqual(rc, 1, out)
        self.assertIn("zero imports compared", out)
        self.assertNotIn("VERIFIED", out)

    def test_recomp_strict_prefix_of_oracle_fails_by_default(self):
        toml = self.toml_with(range(3))
        rc, out = run_tool([toml, self.trace(STUBS[:2]), self.trace(STUBS[:3])])
        self.assertEqual(rc, 1, out)
        self.assertIn("strict prefix of the oracle sequence (2 of 3 imports", out)
        self.assertIn("--allow-prefix", out)
        self.assertNotIn("VERIFIED", out)

    def test_oracle_strict_prefix_of_recomp_fails(self):
        toml = self.toml_with(range(3))
        rc, out = run_tool([toml, self.trace(STUBS[:3]), self.trace(STUBS[:2])])
        self.assertEqual(rc, 1, out)
        self.assertIn("shorter than the recomp sequence (2 of 3 imports", out)
        self.assertNotIn("VERIFIED", out)

    def test_empty_import_table_fails_in_comparison_mode(self):
        toml = self.write_imports([])
        rc, out = run_tool([toml, self.trace([]), self.trace([])])
        self.assertEqual(rc, 2, out)
        self.assertIn("empty import table", out)

    def test_missing_import_table_fails(self):
        toml = self.write_imports([(STUBS[0], NIDS[0])])
        with open(toml, "w", newline="\n") as f:
            f.write('[other]\nkey = 1\n')
        rc, out = run_tool([toml, self.trace([STUBS[0]]), self.trace([STUBS[0]])])
        self.assertEqual(rc, 2, out)
        self.assertIn("missing or non-list top-level 'import' table", out)

    # --- explicit prefix mode ---------------------------------------------

    def test_allow_prefix_accepts_recomp_strict_prefix(self):
        toml = self.toml_with(range(3))
        rc, out = run_tool([toml, self.trace(STUBS[:2]), self.trace(STUBS[:3]), "--allow-prefix"])
        self.assertEqual(rc, 0, out)
        self.assertIn(
            "PREFIX-MATCH (--allow-prefix): recomp sequence is a strict prefix of the "
            "oracle sequence (2 of 3 imports agree)",
            out,
        )

    def test_allow_prefix_accepts_oracle_strict_prefix(self):
        toml = self.toml_with(range(3))
        rc, out = run_tool([toml, self.trace(STUBS[:3]), self.trace(STUBS[:2]), "--allow-prefix"])
        self.assertEqual(rc, 0, out)
        self.assertIn(
            "PREFIX-MATCH (--allow-prefix): oracle sequence is a strict prefix of the "
            "recomp sequence (2 of 3 imports agree)",
            out,
        )

    def test_allow_prefix_still_rejects_divergence(self):
        toml = self.toml_with(range(4))
        rc, out = run_tool([toml, self.trace(STUBS[:2]), self.trace([STUBS[0], STUBS[3]]), "--allow-prefix"])
        self.assertEqual(rc, 1, out)
        self.assertIn("FAILED: import sequences diverge", out)

    def test_allow_prefix_still_rejects_zero_compared(self):
        toml = self.toml_with(range(2))
        rc, out = run_tool([toml, self.trace([]), self.trace([]), "--allow-prefix"])
        self.assertEqual(rc, 1, out)
        self.assertIn("zero imports compared", out)

    def test_allow_prefix_reports_verified_for_exact_match(self):
        toml = self.toml_with(range(2))
        rc, out = run_tool([toml, self.trace(STUBS[:2]), self.trace(STUBS[:2]), "--allow-prefix"])
        self.assertEqual(rc, 0, out)
        self.assertIn("VERIFIED: import sequences identical (2 imports)", out)

    # --- informational mode ------------------------------------------------

    def test_single_trace_informational_mode_exits_zero_without_equivalence_claim(self):
        toml = self.toml_with(range(3))
        rc, out = run_tool([toml, self.trace(STUBS[:3])])
        self.assertEqual(rc, 0, out)
        self.assertIn("sceUtility.0x00000222", out)
        self.assertIn("informational extraction mode: no oracle trace supplied; no equivalence claim made", out)
        self.assertNotIn("VERIFIED", out)
        self.assertNotIn("AGREE", out)
        self.assertNotIn("PREFIX-MATCH", out)

    def test_informational_mode_still_fails_on_malformed_trace(self):
        toml = self.toml_with(range(2))
        rc, out = run_tool([toml, self.trace([STUBS[0]], extra_lines=[""])])
        self.assertEqual(rc, 1, out)
        self.assertIn("malformed trace record", out)

    # --- malformed data ------------------------------------------------------

    def test_malformed_trace_line_fails_in_verification(self):
        toml = self.toml_with(range(2))
        bad = self.trace(STUBS[:2], extra_lines=["7 only two fields"])
        rc, out = run_tool([toml, bad, self.trace(STUBS[:2])])
        self.assertEqual(rc, 1, out)
        self.assertIn("malformed trace record", out)
        self.assertNotIn("VERIFIED", out)

    def test_malformed_pc_field_fails(self):
        toml = self.toml_with(range(2))
        bad = self.trace(STUBS[:1], extra_lines=["7 pc=0xzz op=0x03e00008"])
        rc, out = run_tool([toml, bad, self.trace(STUBS[:1])])
        self.assertEqual(rc, 1, out)
        self.assertIn("malformed pc field", out)

    def test_import_entry_missing_fields_fails(self):
        toml = self.write_imports([(STUBS[0], NIDS[0])])
        with open(toml, "w", newline="\n") as f:
            f.write("[[import]]\nstub = 0x08a00000\nlib = \"sceUtility\"\n")
        rc, out = run_tool([toml, self.trace([STUBS[0]]), self.trace([STUBS[0]])])
        self.assertEqual(rc, 2, out)
        self.assertIn("entry 0 lacks stub/lib/nid fields", out)

    def test_import_entry_non_u32_stub_fails(self):
        toml = self.write_imports([(STUBS[0], NIDS[0])])
        with open(toml, "w", newline="\n") as f:
            f.write("[[import]]\nstub = \"0x08a00000\"\nlib = \"sceUtility\"\nnid = 0x111\n")
        rc, out = run_tool([toml, self.trace([STUBS[0]]), self.trace([STUBS[0]])])
        self.assertEqual(rc, 2, out)
        self.assertIn("non-u32 stub value", out)

    def test_duplicate_stub_address_fails(self):
        toml = self.write_imports([(STUBS[0], NIDS[0])])
        with open(toml, "w", newline="\n") as f:
            for _ in range(2):
                f.write("[[import]]\nstub = 0x08a00000\nlib = \"sceUtility\"\nnid = 0x111\n")
        rc, out = run_tool([toml, self.trace([STUBS[0]]), self.trace([STUBS[0]])])
        self.assertEqual(rc, 2, out)
        self.assertIn("duplicate stub address", out)

    def test_malformed_toml_fails(self):
        toml = self.write_imports([(STUBS[0], NIDS[0])])
        with open(toml, "w", newline="\n") as f:
            f.write("[[import]]\nstup = 1\n")
        rc, out = run_tool([toml, self.trace([STUBS[0]]), self.trace([STUBS[0]])])
        self.assertEqual(rc, 2, out)
        self.assertIn("entry 0 lacks stub/lib/nid fields", out)

    # --- CLI errors ----------------------------------------------------------

    def test_unknown_option_is_a_usage_error(self):
        toml = self.toml_with(range(2))
        rc, out = run_tool([toml, self.trace(STUBS[:1]), "--bogus"])
        self.assertEqual(rc, 2, out)
        self.assertIn("usage: nidseq.py", out)

    def test_wrong_arity_is_a_usage_error(self):
        toml = self.toml_with(range(2))
        t = self.trace(STUBS[:1])
        for argv in ([], [toml], [toml, t, t, t]):
            rc, out = run_tool(argv)
            self.assertEqual(rc, 2, out)
            self.assertIn("usage: nidseq.py", out)

    def test_missing_files_are_usage_errors(self):
        toml = self.toml_with(range(2))
        t = self.trace(STUBS[:1])
        ghost = os.path.join(self.d, "ghost.trace")
        rc, out = run_tool([os.path.join(self.d, "no.toml"), t, t])
        self.assertEqual(rc, 2, out)
        self.assertIn("cannot read import map", out)
        rc, out = run_tool([toml, ghost, t])
        self.assertEqual(rc, 2, out)
        self.assertIn("cannot read trace", out)
        rc, out = run_tool([toml, t, ghost])
        self.assertEqual(rc, 2, out)
        self.assertIn("cannot read oracle trace", out)


class TestNidseqUnit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, TOOLS)
        global nidseq
        import nidseq as m

        cls.m = m

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.d = self._tmp.name

    def write(self, text):
        p = os.path.join(self.d, "imports.toml")
        with open(p, "w", newline="\n") as f:
            f.write(text)
        return p

    def test_import_range_derived_from_stub_min_max(self):
        p = self.write(
            "".join(
                f"[[import]]\nstub = 0x{s:08x}\nlib = \"sceUtility\"\nnid = 0x{n:08x}\n"
                for s, n in zip(STUBS, NIDS, strict=True)
            )
        )
        imp, s0, s1 = self.m.load_imports(p)
        self.assertEqual((s0, s1), (0x08A00000, 0x08A00020))
        self.assertEqual(len(imp), 4)
        self.assertEqual(imp[STUBS[2]], ("sceUtility", NIDS[2]))

    def test_nid_seq_orders_stub_hits_and_skips_fillers(self):
        p = self.write(
            "".join(
                f"[[import]]\nstub = 0x{s:08x}\nlib = \"sceUtility\"\nnid = 0x{n:08x}\n"
                for s, n in zip(STUBS, NIDS, strict=True)
            )
        )
        imp, s0, s1 = self.m.load_imports(p)
        tp = os.path.join(self.d, "t.trace")
        lines = []
        for pc in [STUBS[1], STUBS[0], STUBS[1]]:
            lines.append(f"{len(lines)} pc=0x{pc:08x} op=0x03e00008 r2=0x1")
            lines.append(f"{len(lines)} pc=0x{FILLER_PC:08x} op=0x00000000 r2=0x0")
        with open(tp, "w", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        seq = self.m.nid_seq(tp, imp, s0, s1)
        self.assertEqual([pc for pc, _ in seq], [STUBS[1], STUBS[0], STUBS[1]])
        self.assertEqual([v for _, v in seq], [imp[STUBS[1]], imp[STUBS[0]], imp[STUBS[1]]])

    def test_verify_sequences_exact_prefix_and_divergence(self):
        imp = {STUBS[0]: ("l", 1), STUBS[1]: ("l", 2)}
        mine = [(STUBS[0], imp[STUBS[0]]), (STUBS[1], imp[STUBS[1]])]
        orac = [(STUBS[0], imp[STUBS[0]]), (STUBS[1], imp[STUBS[1]])]
        self.assertEqual(self.m.verify_sequences(mine, orac, imp, allow_prefix=False), 0)
        self.assertEqual(self.m.verify_sequences(mine[:1], orac, imp, allow_prefix=False), 1)
        self.assertEqual(self.m.verify_sequences(mine, orac[:1], imp, allow_prefix=False), 1)
        self.assertEqual(self.m.verify_sequences(mine[:1], orac, imp, allow_prefix=True), 0)
        self.assertEqual(self.m.verify_sequences([], [], imp, allow_prefix=False), 1)


if __name__ == "__main__":
    unittest.main()
