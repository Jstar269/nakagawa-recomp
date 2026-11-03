# SPDX-License-Identifier: GPL-3.0-or-later

"""Carrying runtime bindings to another revision by masked-code matching (#1847).

Every module here is synthetic and source-owned: two "revisions" of one small program
are assembled from word lists, laid out at different addresses, and analysed by the real
``codegen.py --cfg-report``. The second revision inserts and reorders functions,
changes one body, drops one function and adds an ambiguous decoy, which is what a title
update does to a real executable.
"""

from __future__ import annotations

import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import binding_carry  # noqa: E402
import title_manifest  # noqa: E402
from test_analysis_ir import BASE, JR_RA, NOP, jal, write_elf  # noqa: E402

LUI_A0 = 0x3C040880      # lui a0, 0x0880
ADDIU_A0 = 0x24841234    # addiu a0, a0, 0x1234
LW_GP = 0x8F820010       # lw v0, 0x10(gp)


def body(seed: int, length: int = 10) -> list:
    """A leaf function whose words depend on ``seed`` and nothing else."""
    words = [0x24020000 | ((seed * 16 + index) & 0xFFFF) for index in range(length)]
    return words + [JR_RA, NOP]


def address_user(high: int, low: int, gp_offset: int) -> list:
    """A body that forms an address (lui/addiu) and reads a $gp slot: both must be masked."""
    return [0x3C040000 | high, 0x24840000 | low, 0x8F820000 | gp_offset] + body(40)


# Revision A: U1 (address user), H1 and H2 (identical helpers), U2, U3, EDIT, GONE.
REVISION_A = {
    "main": ["call:U1", "call:H1", "call:U2", "call:H2", "call:U3", "call:EDIT", "call:GONE", JR_RA, NOP],
    "U1": address_user(0x0880, 0x1234, 0x0010),
    "H1": body(7, 6),
    "U2": body(2),
    "H2": body(7, 6),
    "U3": body(3),
    "EDIT": body(4, 14),
    "GONE": body(5),
}
EDITED = body(4, 14)
EDITED[11] = 0x24020999   # one word of the body changes

# Revision B: new functions in between, a different order, U1's address and $gp offset
# differ, H1/H2 swap places relative to their neighbours, EDIT changed, GONE removed.
REVISION_B = {
    "main": ["call:U2", "call:H2", "call:U3", "call:INSERTED", "call:FILLER", "call:U1", "call:H1", "call:EDIT",
             JR_RA, NOP],
    "INSERTED": body(90),
    "U2": body(2),
    "H2": body(7, 6),
    "U3": body(3),
    "FILLER": body(91),
    "U1": address_user(0x0890, 0x5678, 0x0020),
    "H1": body(7, 6),
    "EDIT": EDITED,
}


def layout(functions: dict, base: int = BASE) -> tuple[list, dict]:
    """Words of the whole image and each function's address."""
    addresses, cursor = {}, base
    for name, words in functions.items():
        addresses[name] = cursor
        cursor += len(words) * 4
    image = []
    for words in functions.values():
        for word in words:
            image.append(jal(addresses[word[5:]]) if isinstance(word, str) else word)
    return image, addresses


class Fixture:
    def __init__(self, root: Path):
        self.root = root

    def analyse(self, name: str, functions: dict) -> tuple[Path, dict]:
        image, addresses = layout(functions)
        elf = self.root / f"{name}.elf"
        write_elf(elf, image)
        out = self.root / name
        out.mkdir()
        report = out / "cfg.json"
        proc = subprocess.run(
            [sys.executable, str(TOOLS / "codegen.py"), str(elf), str(out / "out.c"), "--base=0",
             "--profile=none", "--funcs-per-chunk=2000", f"--cfg-report={report}"],
            cwd=ROOT, capture_output=True, text=True, check=False)
        if proc.returncode:
            raise AssertionError(proc.stdout + proc.stderr)
        return report, addresses


class Revisions:
    """Analyses both revisions once per test class."""

    _tmp: tempfile.TemporaryDirectory
    root: Path
    old_cfg: Path
    new_cfg: Path
    old: dict[str, int]
    new: dict[str, int]

    @classmethod
    def setUpClass(cls) -> None:
        cls._tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls._tmp.name)
        fixture = Fixture(cls.root)
        cls.old_cfg, cls.old = fixture.analyse("a", REVISION_A)
        cls.new_cfg, cls.new = fixture.analyse("b", REVISION_B)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()


class CarryTests(Revisions, unittest.TestCase):
    def matcher(self, old_cfg=None, new_cfg=None) -> binding_carry.Matcher:
        old = binding_carry.load_module(old_cfg or self.old_cfg, None)
        new = binding_carry.load_module(new_cfg or self.new_cfg, None)
        matcher = binding_carry.Matcher(old, new, binding_carry.FUZZY_MIN_SIMILARITY, binding_carry.FUZZY_MARGIN)
        matcher.run()
        return matcher

    def test_addresses_really_moved(self):
        self.assertNotEqual(self.old["U1"], self.new["U1"])
        self.assertNotEqual(self.old["U2"], self.new["U2"])

    def test_exact_matches_ignore_relocated_addresses_and_gp_offsets(self):
        matcher = self.matcher()
        for name in ("U1", "U2", "U3"):
            status = matcher.status(self.old[name])
            self.assertEqual(status["status"], "exact", name)
            self.assertEqual(status["new"], self.new[name], name)

    def test_identical_helpers_are_told_apart_by_their_neighbours(self):
        matcher = self.matcher()
        self.assertEqual(matcher.status(self.old["H1"]), {"status": "neighbour", "new": self.new["H1"],
                                                          "similarity": 1.0})
        self.assertEqual(matcher.status(self.old["H2"])["new"], self.new["H2"])
        self.assertEqual(matcher.status(self.old["H2"])["status"], "neighbour")

    def test_edited_function_is_fuzzy_with_its_similarity(self):
        status = self.matcher().status(self.old["EDIT"])
        self.assertEqual(status["status"], "fuzzy")
        self.assertEqual(status["new"], self.new["EDIT"])
        self.assertGreaterEqual(status["similarity"], 0.75)
        self.assertLess(status["similarity"], 1.0)

    def test_deleted_function_is_unmatched_with_its_closest_candidate(self):
        status = self.matcher().status(self.old["GONE"])
        self.assertEqual(status["status"], "unmatched")
        self.assertIn("closest", status)
        self.assertLess(status["similarity"], 0.75)

    def test_a_masked_constant_that_is_not_an_address_still_distinguishes_functions(self):
        # U2 and U3 differ only in an immediate that is not a relocation, so they never merge.
        matcher = self.matcher()
        self.assertNotEqual(matcher.status(self.old["U2"])["new"], matcher.status(self.old["U3"])["new"])


class ManifestTests(Revisions, unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        old = cls.old
        bindings = {
            "schema_version": 1,
            "fallback_entry": old["U1"],
            "worker_thread_entry": old["EDIT"],
            "launcher_thread_entry": old["GONE"],
            "dispatch_aliases": [{"from": old["H1"], "to": old["H2"]}],
        }
        manifest = json.loads((ROOT / "assets/titles/display-smoke.json").read_text(encoding="utf-8"))
        manifest["runtime_bindings"] = bindings
        cls.bindings = bindings
        cls.manifest_path = cls.root / "title.json"
        cls.manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    def args(self, **overrides):
        values = dict(old_cfg=str(self.old_cfg), new_cfg=str(self.new_cfg), manifest=str(self.manifest_path),
                      old_imports=None, new_imports=None, accept_fuzzy=False,
                      min_similarity=binding_carry.FUZZY_MIN_SIMILARITY, margin=binding_carry.FUZZY_MARGIN,
                      out_bindings=None, report=None)
        values.update(overrides)
        return type("Args", (), values)()

    def rows(self, report):
        return {row["name"]: row for row in report["bindings"]}

    def test_only_exact_class_matches_are_written_without_accept_fuzzy(self):
        code, report = binding_carry.run(self.args())
        rows = self.rows(report)
        self.assertEqual(rows["fallback_entry"]["new"], self.new["U1"])
        self.assertEqual(rows["worker_thread_entry"]["status"], "fuzzy")
        self.assertEqual(rows["launcher_thread_entry"]["status"], "unmatched")
        proposal = report["proposed_runtime_bindings"]
        self.assertEqual(proposal["fallback_entry"], self.new["U1"])
        self.assertNotIn("worker_thread_entry", proposal)
        self.assertNotIn("launcher_thread_entry", proposal)
        self.assertEqual(proposal["dispatch_aliases"], [{"from": self.new["H1"], "to": self.new["H2"]}])
        self.assertEqual(code, 1, "an uncarried binding must make the run fail loudly")
        title_manifest.validate_runtime_bindings(proposal, "$.runtime_bindings")

    def test_accept_fuzzy_writes_the_fuzzy_match_but_never_the_unmatched_one(self):
        _, report = binding_carry.run(self.args(accept_fuzzy=True))
        proposal = report["proposed_runtime_bindings"]
        self.assertEqual(proposal["worker_thread_entry"], self.new["EDIT"])
        self.assertNotIn("launcher_thread_entry", proposal)

    def test_a_dropped_pair_end_drops_the_whole_alias_and_says_so(self):
        bindings = copy.deepcopy(self.bindings)
        bindings["dispatch_aliases"] = [{"from": self.old["H1"], "to": self.old["GONE"]}]
        manifest = json.loads(self.manifest_path.read_text(encoding="utf-8"))
        manifest["runtime_bindings"] = bindings
        path = self.root / "title-dropped.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        _, report = binding_carry.run(self.args(manifest=str(path)))
        proposal = report["proposed_runtime_bindings"]
        self.assertNotIn("dispatch_aliases", proposal)
        self.assertTrue(any("dispatch_aliases[0]" in note for note in report["notes"]))

    def test_an_interior_address_is_carried_only_where_the_code_lines_up(self):
        matcher = binding_carry.Matcher(binding_carry.load_module(self.old_cfg, None),
                                        binding_carry.load_module(self.new_cfg, None), 0.75, 0.05)
        matcher.run()
        inside_edit_before_change = self.old["EDIT"] + 8
        edited_word = self.old["EDIT"] + 11 * 4
        rows = binding_carry.carry_sites(matcher, [
            {"name": "before", "address": inside_edit_before_change},
            {"name": "edited", "address": edited_word},
            {"name": "nowhere", "address": 0x20000000}])
        by_name = {row["name"]: row for row in rows}
        self.assertEqual(by_name["before"]["status"], "fuzzy")
        self.assertEqual(by_name["before"]["new"], self.new["EDIT"] + 8)
        self.assertEqual(by_name["edited"]["status"], "unmatched")
        self.assertEqual(by_name["nowhere"]["status"], "unmatched")

    def test_the_manifest_is_never_rewritten_and_the_cli_reports_exit_codes(self):
        before = hashlib.sha256(self.manifest_path.read_bytes()).hexdigest()
        out = self.root / "out.json"
        report = self.root / "report.json"
        argv = ["--old-cfg", str(self.old_cfg), "--new-cfg", str(self.new_cfg), "--manifest", str(self.manifest_path),
                "--out-bindings", str(out), "--report", str(report)]
        with contextlib.redirect_stderr(io.StringIO()) as stderr:
            code = binding_carry.main(argv)
        self.assertEqual(code, 1)
        self.assertEqual(hashlib.sha256(self.manifest_path.read_bytes()).hexdigest(), before)
        self.assertEqual(json.loads(out.read_text(encoding="utf-8"))["fallback_entry"], self.new["U1"])
        self.assertEqual(json.loads(report.read_text(encoding="utf-8"))["private"], True)
        for token in ("EXACT", "NEIGHBOUR", "FUZZY", "UNMATCHED", "CHECK BY HAND", "CARRY_INCOMPLETE"):
            self.assertIn(token, stderr.getvalue())
        with contextlib.redirect_stderr(io.StringIO()) as stderr:
            self.assertEqual(binding_carry.main(["--old-cfg", "nope.json", "--new-cfg", str(self.new_cfg),
                                                 "--manifest", str(self.manifest_path)]), 2)
        self.assertIn("CARRY_INPUT", stderr.getvalue())


class RefusalTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.fixture = Fixture(Path(self._tmp.name))

    def match(self, old_functions, new_functions):
        old_cfg, old = self.fixture.analyse("o", old_functions)
        new_cfg, new = self.fixture.analyse("n", new_functions)
        matcher = binding_carry.Matcher(binding_carry.load_module(old_cfg, None),
                                        binding_carry.load_module(new_cfg, None), 0.75, 0.05)
        matcher.run()
        return matcher, old, new

    def test_a_decoy_that_neighbours_cannot_separate_is_ambiguous_and_never_guessed(self):
        old = {"main": ["call:X", "call:D", JR_RA, NOP], "X": body(1), "D": body(2)}
        new = {"main": ["call:X", "call:D", "call:D2", JR_RA, NOP], "X": body(1), "D": body(2), "D2": body(2)}
        matcher, old_a, new_a = self.match(old, new)
        status = matcher.status(old_a["D"])
        self.assertEqual(status["status"], "ambiguous")
        self.assertEqual(status["candidates"], sorted([new_a["D"], new_a["D2"]]))
        self.assertNotIn(old_a["D"], matcher.match)

    def test_a_vote_from_exact_callers_carries_a_function_too_changed_to_match_fuzzily(self):
        changed = body(6, 12)
        for index in (1, 3, 5, 7, 9):
            changed[index] = 0x24030000 | index
        old = {"main": ["call:F", JR_RA, NOP], "F": body(6, 12)}
        new = {"main": ["call:F", JR_RA, NOP], "F": changed}
        matcher, old_a, new_a = self.match(old, new)
        status = matcher.status(old_a["F"])
        self.assertEqual(status["status"], "caller")
        self.assertEqual(status["new"], new_a["F"])
        self.assertLess(status["similarity"], 0.75)

    def test_a_rewritten_function_is_not_paired_by_its_caller_alone(self):
        old = {"main": ["call:F", JR_RA, NOP], "F": body(6, 12)}
        new = {"main": ["call:F", JR_RA, NOP], "F": [0x00851021] * 12 + [JR_RA, NOP]}
        matcher, old_a, _ = self.match(old, new)
        status = matcher.status(old_a["F"])
        self.assertEqual(status["status"], "unmatched")
        self.assertNotIn(old_a["F"], matcher.match)

    def test_import_stubs_with_identical_bodies_match_by_nid(self):
        stub = body(8, 6)
        old = {"main": ["call:S1", "call:S2", JR_RA, NOP], "S1": stub, "S2": stub}
        new = {"main": ["call:S2", "call:S1", JR_RA, NOP], "S2": stub, "S1": stub}
        old_cfg, old_a = self.fixture.analyse("o", old)
        new_cfg, new_a = self.fixture.analyse("n", new)
        old_module = binding_carry.Module(json.loads(old_cfg.read_text(encoding="utf-8")),
                                          {old_a["S1"]: "0xaaaa0001", old_a["S2"]: "0xbbbb0002"})
        new_module = binding_carry.Module(json.loads(new_cfg.read_text(encoding="utf-8")),
                                          {new_a["S1"]: "0xaaaa0001", new_a["S2"]: "0xbbbb0002"})
        blind = binding_carry.Matcher(binding_carry.Module(json.loads(old_cfg.read_text(encoding="utf-8"))),
                                      binding_carry.Module(json.loads(new_cfg.read_text(encoding="utf-8"))), 0.75, 0.05)
        blind.run()
        self.assertEqual(blind.status(old_a["S1"])["status"], "ambiguous", "without NIDs the stubs are twins")
        matcher = binding_carry.Matcher(old_module, new_module, 0.75, 0.05)
        matcher.run()
        self.assertEqual(matcher.status(old_a["S1"]), {"status": "exact", "new": new_a["S1"], "similarity": 1.0})
        self.assertEqual(matcher.status(old_a["S2"])["new"], new_a["S2"])

    def test_a_manifest_without_bindings_is_refused(self):
        path = Path(self._tmp.name) / "plain.json"
        path.write_text((ROOT / "assets/titles/display-smoke.json").read_text(encoding="utf-8"), encoding="utf-8")
        args = type("Args", (), dict(old_cfg="a", new_cfg="b", manifest=str(path), old_imports=None,
                                     new_imports=None, accept_fuzzy=False, min_similarity=0.75, margin=0.05))()
        with self.assertRaises(binding_carry.BindingCarryError) as raised:
            binding_carry.run(args)
        self.assertEqual(raised.exception.code, "CARRY_MANIFEST")


if __name__ == "__main__":
    unittest.main()
