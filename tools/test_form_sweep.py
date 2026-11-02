# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Pin the form sweep's census, aliasing patterns, immediate buckets and mutant bookkeeping (#1844).

The execution half (the translated body against the reference interpreter) is
``mingw32-make form-sweep-selftest``; these tests cover the Python half without a compiler.
"""

from __future__ import annotations

import contextlib
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import codegen  # noqa: E402
import form_sweep as fs  # noqa: E402


class ImmediateBuckets(unittest.TestCase):
    def test_exact_values(self):
        for value in (0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 24, 31, 32):
            self.assertEqual(fs.imm_bucket(value, 16), f"={value}")
            self.assertEqual(fs.imm_bucket(value, 5) if value < 32 else f"={value}", f"={value}")

    def test_sixteen_bit_ranges(self):
        self.assertEqual(fs.imm_bucket(9, 16), "small")
        self.assertEqual(fs.imm_bucket(0x0FF, 16), "small")
        self.assertEqual(fs.imm_bucket(0x100, 16), "large")
        self.assertEqual(fs.imm_bucket(0x1234, 16), "large")
        self.assertEqual(fs.imm_bucket(0x7FFF, 16), "max")
        self.assertEqual(fs.imm_bucket(0x8000, 16), "min")
        self.assertEqual(fs.imm_bucket(0x8001, 16), "neg-large")
        self.assertEqual(fs.imm_bucket(0xC000, 16), "neg-large")
        self.assertEqual(fs.imm_bucket(0xFF00, 16), "neg-small")
        self.assertEqual(fs.imm_bucket(0xFFFE, 16), "neg-small")
        self.assertEqual(fs.imm_bucket(0xFFFF, 16), "-1")

    def test_short_fields_are_unsigned_counts(self):
        self.assertEqual(fs.imm_bucket(9, 5), "low")
        self.assertEqual(fs.imm_bucket(15, 5), "low")
        self.assertEqual(fs.imm_bucket(17, 5), "high")
        self.assertEqual(fs.imm_bucket(30, 5), "high")


class AliasPatterns(unittest.TestCase):
    def test_one_operand(self):
        self.assertEqual(fs.alias_patterns(1), [(0,), ("Z",)])

    def test_two_operands(self):
        self.assertEqual(fs.alias_patterns(2),
                         [(0, 0), (0, 1), (0, "Z"), ("Z", 0), ("Z", "Z")])

    def test_three_operands_name_every_aliasing(self):
        patterns = fs.alias_patterns(3)
        self.assertEqual(len(patterns), len(set(patterns)))
        # (rd, rt, rs) in first-appearance renaming: each equality class once.
        for expected in ((0, 0, 0), (0, 0, 1), (0, 1, 0), (0, 1, 1), (0, 1, 2),
                         ("Z", 0, 0), (0, "Z", 0), (0, 0, "Z"), ("Z", "Z", "Z"), (0, "Z", 1)):
            self.assertIn(expected, patterns)
        # A fresh register is only ever the next unused name; the zero register may repeat.
        for p in patterns:
            seen = -1
            for c in p:
                if c != "Z":
                    self.assertLessEqual(c, seen + 1)
                    seen = max(seen, c)

    def test_base_register_forbids_zero(self):
        for p in fs.alias_patterns(3, (True, True, False)):
            self.assertNotEqual(p[2], "Z")


class Census(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.census = fs.census()
        cls.by_name = {}
        for f in cls.census.forms:
            cls.by_name.setdefault(f.name, []).append(f)

    def test_forms_come_from_codegen(self):
        names = set(self.by_name)
        for expected in ("addu", "subu", "slt", "sllv", "sll", "rotr", "mult", "div", "mfhi",
                         "addiu", "lui", "ext", "ins", "seb", "lw", "sw", "lwl", "swr", "ll", "sc",
                         "lwc1", "swc1", "mtc1", "mfc1", "ctc1", "cfc1", "add.s", "mul.s",
                         "cvt.w.s", "cvt.s.w", "c.eq.s", "movz", "clz", "max", "madd", "msubu"):
            self.assertIn(expected, names, expected)
        self.assertGreater(len(self.census.forms), 90)

    def test_operand_roles(self):
        addu = self.by_name["addu"][0]
        self.assertEqual(addu.regs, {"rd": "gpr", "rs": "gpr", "rt": "gpr"})
        self.assertEqual(addu.imms, ())
        self.assertFalse(addu.memory)
        sll = self.by_name["sll"][0]
        self.assertEqual(sll.regs, {"rd": "gpr", "rt": "gpr"})
        self.assertEqual(sll.imms, ("sa",))
        mtc1 = self.by_name["mtc1"][0]
        self.assertEqual(mtc1.regs, {"rd": "fpr", "rt": "gpr"})
        addiu = self.by_name["addiu"][0]
        self.assertEqual(addiu.regs, {"rt": "gpr", "rs": "gpr"})
        self.assertEqual(addiu.imms, ("imm",))
        add_s = self.by_name["add.s"][0]
        self.assertEqual(add_s.regs, {"sa": "fpr", "rd": "fpr", "rt": "fpr"})
        sqrt_s = self.by_name["sqrt.s"][0]
        self.assertEqual(sqrt_s.regs, {"sa": "fpr", "rd": "fpr"})

    def test_memory_forms_use_rs_as_the_base(self):
        for name in ("lw", "sw", "lwl", "swr", "ll", "sc", "lwc1", "swc1", "lb", "sh"):
            self.assertTrue(self.by_name[name][0].memory, name)
        self.assertFalse(self.by_name["addiu"][0].memory)

    def test_rotate_is_its_own_form_selected_by_a_field(self):
        srl_forms = self.by_name["srl"]
        rotr_forms = self.by_name["rotr"]
        self.assertTrue(srl_forms and rotr_forms)
        self.assertEqual(srl_forms[0].fixed.get("rs"), 0)
        self.assertEqual(rotr_forms[0].fixed.get("rs"), 1)

    def test_excluded_forms_are_named(self):
        excluded = {n for names in self.census.excluded.values() for n in names}
        self.assertTrue({"syscall", "break"} <= excluded)
        self.assertNotIn("syscall", self.by_name)

    def test_vfpu_words_are_not_scalar_forms(self):
        self.assertTrue(fs.probe(fs.word_of(0x12, rs=7, rt=8)).vfpu)  # mtv
        self.assertTrue(all(f.op not in (0x18, 0x19, 0x1B, 0x32, 0x34, 0x35, 0x36, 0x37, 0x3A,
                                         0x3C, 0x3D, 0x3E) for f in self.census.forms))

    def test_a_form_added_to_codegen_appears_in_the_census(self):
        original = codegen.effect

        def with_new_form(addr, w, *args, **kwargs):
            if (w >> 26) == 0x1C and (w & 0x3F) == 0x05:      # an invented SPECIAL2 form
                a, b, d = codegen.rs(w), codegen.rt(w), codegen.rd(w)
                return codegen.wr(d, f"({codegen.R(a)} ^ ~{codegen.R(b)})"), None, 0
            return original(addr, w, *args, **kwargs)

        codegen.effect = with_new_form
        fs.probe.cache_clear()
        try:
            found = fs.census()
            self.assertTrue(fs.is_r_like(0x1C))
            self.assertFalse(fs.is_r_like(0x09))
        finally:
            codegen.effect = original
            fs.probe.cache_clear()
        new = [f for f in found.forms if f.op == 0x1C]
        self.assertEqual(len(new), 1)
        self.assertEqual(new[0].regs, {"rd": "gpr", "rs": "gpr", "rt": "gpr"})
        self.assertEqual(new[0].fixed, {"funct": 5})
        # ... and it expands into instances without any edit to the sweep.
        instances, _ = fs.form_instances(new[0], seed=1, cap=None)
        self.assertEqual(len(instances), len(fs.alias_patterns(3)))


class Instances(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.census = fs.census()
        cls.forms = {f.name: f for f in cls.census.forms}

    def test_the_generator_emits_no_reserved_bitfield_encoding(self):
        # ext with lsb + msbd > 31 and ins with msb < lsb are UNPREDICTABLE; the reference
        # refuses them, so a generated one would be a NOT_CHECKED instance of a live form.
        self.assertTrue(fs.reserved_encoding(0x7C080FC0))          # ext, lsb 31, msbd 1
        self.assertFalse(fs.reserved_encoding(0x7C08F800))         # ext, lsb 0, msbd 31
        self.assertTrue(fs.reserved_encoding(0x7C080004 | (5 << 6)))  # ins, msb 0 < lsb 5
        self.assertFalse(fs.reserved_encoding(0x7C08F804))         # ins, msb 31, lsb 0
        for name in ("ext", "ins"):
            if name not in self.forms:
                continue
            instances, _total = fs.form_instances(self.forms[name], seed=1, cap=None)
            self.assertTrue(instances, name)
            for inst in instances:
                self.assertFalse(fs.reserved_encoding(inst.word), f"{inst.word:#010x}")
        self.assertIn("ext", self.forms)

    def test_addu_covers_every_aliasing_pattern_once(self):
        instances, total = fs.form_instances(self.forms["addu"], seed=1, cap=None)
        self.assertEqual(total, len(fs.alias_patterns(3)))
        self.assertEqual(len(instances), total)
        seen = set()
        for inst in instances:
            rd, rs, rt = (inst.regs[n] for n in ("rd", "rs", "rt"))
            seen.add((rd == rs, rd == rt, rs == rt, rd == 0, rs == 0, rt == 0))
        self.assertIn((True, False, False, False, False, False), seen)    # rd == rs
        self.assertIn((False, True, False, False, False, False), seen)    # rd == rt
        self.assertIn((False, False, True, False, False, False), seen)    # rs == rt
        self.assertIn((False, False, False, True, False, False), seen)    # rd is $zero
        self.assertIn((False, False, False, False, True, False), seen)    # rs is $zero

    def test_register_numbers_are_valid_and_never_ra(self):
        for form in self.census.forms:
            instances, _ = fs.form_instances(form, seed=3, cap=12)
            for inst in instances:
                self.assertEqual(inst.word >> 26, form.op)
                for reg in inst.regs.values():
                    self.assertTrue(0 <= reg < 31, (form.name, inst.pattern))

    def test_memory_base_is_never_zero(self):
        for form in self.census.forms:
            if not form.memory:
                continue
            instances, _ = fs.form_instances(form, seed=3, cap=None)
            self.assertTrue(instances, form.name)
            for inst in instances:
                self.assertGreater(inst.base_reg, 0, form.name)

    def test_cap_samples_but_keeps_every_pattern_and_bucket(self):
        form = self.forms["addiu"]
        full, total = fs.form_instances(form, seed=5, cap=None)
        sampled, total2 = fs.form_instances(form, seed=5, cap=12)
        self.assertEqual(total, total2)
        self.assertLess(len(sampled), len(full))
        patterns = {tuple(i.slots.get(n) == "$zero" for n in ("rt", "rs")) +
                    (i.regs["rt"] == i.regs["rs"],) for i in full}
        got = {tuple(i.slots.get(n) == "$zero" for n in ("rt", "rs")) +
               (i.regs["rt"] == i.regs["rs"],) for i in sampled}
        self.assertEqual(patterns, got)
        buckets = {fs.imm_bucket(i.imm[0], 16) for i in full}
        self.assertEqual(buckets, {fs.imm_bucket(i.imm[0], 16) for i in sampled})

    def test_generation_is_deterministic(self):
        a, _ = fs.all_instances(self.census, 7, 8)
        b, _ = fs.all_instances(self.census, 7, 8)
        self.assertEqual([(i.word, i.pattern) for i in a], [(i.word, i.pattern) for i in b])
        c, _ = fs.all_instances(self.census, 8, 8)
        self.assertNotEqual([i.word for i in a], [i.word for i in c])

    def test_plan_and_module_agree(self):
        instances, totals = fs.all_instances(self.census, 7, 6, only={"00.21", "23"})
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            self.assertEqual(fs.generate(out, 7, 6, 8, {"00.21", "23"}), 0)
            plan = (out / "plan.txt").read_text().splitlines()
            man = json.loads((out / "manifest.json").read_text())
            self.assertEqual(len(man["instances"]), len(instances))
            self.assertEqual(sum(1 for ln in plan if ln.startswith("inst ")), len(instances))
            self.assertIn("states 8", plan)
            prx = (out / "guest.prx").read_bytes()
            self.assertEqual(prx[:4], b"ELF")
            text = prx[fs.TEXT_FILE_OFFSET:]
            placed = {}
            for line in plan:
                if line.startswith("inst "):
                    _tag, index, addr, word, *_rest = line.split()
                    placed[int(index)] = (int(addr, 16), int(word, 16))
            self.assertEqual(len(placed), len(instances))
            for rec in man["instances"]:
                addr, word = placed[rec["index"]]
                self.assertEqual(word, rec["word"])
                self.assertEqual(addr % 16, 0)
                offset = addr - fs.MODULE_BASE
                body = struct.unpack_from("<3I", text, offset)
                self.assertEqual(body, (word, fs.JR_RA, 0) if not rec["delay"] else (fs.JR_RA, word, 0))


class Reporting(unittest.TestCase):
    def test_verdict_grammar(self):
        text = ("INST 0 MATCH states=64\n"
                "INST 1 MISMATCH states=64 bad=3 first=2 r9 aot=0x1 ref=0x2\n"
                "INST 2 NOT_CHECKED ref-unimplemented pc=0x08c00000 op=0x12345678 state=0\n"
                "noise\n")
        got = fs.parse_verdicts(text)
        self.assertEqual(got[0], ("MATCH", "states=64"))
        self.assertEqual(got[1][0], "MISMATCH")
        self.assertEqual(got[2][0], "NOT_CHECKED")
        self.assertEqual(len(got), 3)

    def test_unchecked_form_is_named_and_missing_instance_fails(self):
        man = {"seed": 1, "states": 4, "excluded": {},
               "forms": [{"key": "a", "name": "alpha"}, {"key": "b", "name": "beta"},
                         {"key": "c", "name": "gamma"}],
               "instances": [
                   {"index": 0, "form": "a", "word": 1, "pattern": "p"},
                   {"index": 1, "form": "b", "word": 2, "pattern": "p"},
                   {"index": 2, "form": "c", "word": 3, "pattern": "p"}]}
        verdicts = {0: ("MATCH", ""), 1: ("NOT_CHECKED", "ref-unimplemented")}
        lines, totals = fs.summarize(man, verdicts)
        self.assertTrue(lines[0].startswith("MATCH"))
        self.assertTrue(lines[1].startswith("NOT_CHECKED"))
        self.assertIn("beta", lines[1])
        self.assertTrue(lines[2].startswith("MISSING"))
        self.assertEqual(totals["MISSING"], 1)
        self.assertEqual(totals["forms_unchecked"], 1)

    def test_a_translated_side_failure_fails_the_report_even_beside_a_match(self):
        man = {"seed": 1, "states": 4, "excluded": {},
               "forms": [{"key": "a", "name": "alpha"}],
               "instances": [
                   {"index": 0, "form": "a", "word": 1, "pattern": "p"},
                   {"index": 1, "form": "a", "word": 2, "pattern": "q"}]}
        text = ("INST 0 MATCH states=4\n"
                "INST 1 AOT_FAILED aot-dispatch-error code=-1000 op=0x00000002 state=0\n")
        verdicts = fs.parse_verdicts(text)
        self.assertEqual(verdicts[1][0], "AOT_FAILED")
        lines, totals = fs.summarize(man, verdicts)
        self.assertTrue(lines[0].startswith("AOT_FAILED"))
        self.assertEqual(totals["AOT_FAILED"], 1)
        with tempfile.TemporaryDirectory() as tmp:
            manifest, results = Path(tmp, "m.json"), Path(tmp, "r.txt")
            manifest.write_text(json.dumps(man))
            results.write_text(text)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(fs.report(manifest, results), 1)

    def test_not_checked_outside_the_pinned_set_fails_even_beside_a_match(self):
        man = {"seed": 1, "states": 4, "excluded": {},
               "forms": [{"key": "a", "name": "addu"}, {"key": "s", "name": "sync"}],
               "instances": [
                   {"index": 0, "form": "a", "word": 1, "pattern": "p"},
                   {"index": 1, "form": "a", "word": 2, "pattern": "q"},
                   {"index": 2, "form": "s", "word": 3, "pattern": "p"}]}
        text = ("INST 0 MATCH states=4\n"
                "INST 1 NOT_CHECKED ref-unimplemented pc=0x0 op=0x2 state=0\n"
                "INST 2 NOT_CHECKED ref-unimplemented pc=0x0 op=0x3 state=0\n")
        verdicts = fs.parse_verdicts(text)
        self.assertEqual(fs.unexpected_not_checked(man, verdicts), {"addu": 1})
        with tempfile.TemporaryDirectory() as tmp:
            manifest, results = Path(tmp, "m.json"), Path(tmp, "r.txt")
            manifest.write_text(json.dumps(man))
            results.write_text(text)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(fs.report(manifest, results), 1)
            self.assertIn("addu x1", out.getvalue())
            # The pinned forms alone still pass.
            results.write_text("INST 0 MATCH states=4\nINST 1 MATCH states=4\n"
                               "INST 2 NOT_CHECKED ref-unimplemented pc=0x0 op=0x3 state=0\n")
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(fs.report(manifest, results), 0)


class Mutant(unittest.TestCase):
    def test_expected_set_is_the_aliased_instances(self):
        census = fs.census()
        instances, _ = fs.all_instances(census, 1, None, only={fs.MUTANT_FORM})
        man = {"instances": [{"index": i.index, "form": i.form.key, "regs": i.regs,
                              "delay": i.delay} for i in instances]}
        expected = fs.mutant_expected(man)
        self.assertTrue(expected)
        for i in instances:
            r = i.regs
            self.assertEqual(i.index in expected,
                             r["rd"] != 0 and r["rd"] == r["rs"] and r["rt"] != r["rs"])

    def test_mutant_wrapper_changes_only_addu(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "mutant_codegen.py"
            fs.write_mutant(out)
            source = out.read_text()
        self.assertIn("(w & 0x3F) == 0x21", source)
        compile(source, "mutant_codegen.py", "exec")


if __name__ == "__main__":
    unittest.main()
