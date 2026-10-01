# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for the SR_NAN_TRAP NaN/Inf origin diagnostic (issue #69).

The trap names the FPU/VFPU instruction that turned all-finite operands into a
NaN or an Inf -- the place where this implementation and PSP hardware can
disagree, and therefore where a guest whose bone matrices go NaN must have its
first bad value.

Three claims are proved here, all against the real code, never a re-implementation:

1. Shape. tools/codegen.py with --nan-trap emits an SR_NAN_TRAP_* check after
   every FPU and VFPU result write, carrying the guest PC, the mnemonic, the
   destination register and the operand variables.
2. Zero cost. Over a real source-owned guest (fixtures/cosim), the generated C
   is byte-identical with and without --nan-trap, and the object compiled from
   the untrapped chunk is byte-identical with and without -DSR_NAN_TRAP. The
   flag costs nothing when it is off.
3. Behaviour. A finite -> non-finite result is reported with its exact PC and
   mnemonic; a non-finite result whose operands were already non-finite is NOT
   reported (the trap names the origin, not every echo); a finite result is not
   reported; SR_NAN_TRAP_LIMIT bounds the output. The VFPU lane forms and the
   single-instruction interpreter (src/rt/vfpu_interp.c) are covered too.
4. Operands and frame. `in=` carries EVERY operand the instruction consumed --
   for the matrix forms the matrix lanes AND the vector lanes, which is what
   lets a partial gather stop calling a propagation an origin -- and every line
   carries the guest VBLANK count, so a report lines up with the
   SR_GE_TRANSITION_TRACE frame that showed its effect. Both tiers (generated C
   and the AOT-gap interpreter) gather the same operand list.

Every C fixture below is generated into a temporary directory and links the real
src/rt/debug.c reporter; the only stubs are host symbols those fixtures never
reach (SDL ticks, GE status, the debug watchpoint file reader) and, for the
interpreter fixture, the runtime kernels the exercised instruction does not call.
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import codegen  # noqa: E402

RT = ROOT / "src" / "rt"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
PY = sys.executable
COSIM_GENERATOR = ROOT / "fixtures" / "cosim" / "generate.py"
COSIM_BASE = "0x08900000"

# COP1 word: opcode 0x11, fmt in rs, ft, fs, fd in sa, funct in bits 5:0.
def cop1(fmt: int, ft: int, fs: int, fd: int, funct: int) -> int:
    return (0x11 << 26) | (fmt << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct


# VFPU1/0 lane word: opcode, sub in bits 25:23, vt/vs/vd, size in bits 7 and 15
# (the same decode codegen.vec_size applies).
def vfpu_lane(op: int, sub: int, vt: int, vs: int, vd: int, size: int = 1) -> int:
    size_bits = {1: 0, 2: 1 << 7, 3: 1 << 15, 4: (1 << 7) | (1 << 15)}[size]
    return ((op << 26) | (sub << 23) | (vt << 16) | (vs << 8) | vd | size_bits)


class nan_trap_codegen_state:
    """Turn --nan-trap on for the duration of a block and restore it after."""

    def __enter__(self):
        self._previous = codegen.NAN_TRAP
        codegen.NAN_TRAP = True
        return self

    def __exit__(self, *exc):
        codegen.NAN_TRAP = self._previous
        return False


# The gathered-operand block a matrix form's check is wrapped in. Removing it
# must leave exactly the statement the untrapped generator emits, which is the
# zero-cost claim stated at the level where the arrays actually exist.
GATHER_BLOCK = re.compile(r"\{ float _ntin\[.*?\}")
# The result stores a matrix form emits after its check. They are outputs, not
# operands, so they are not part of what the check gathers.
MATRIX_WRITES = re.compile(r"s->v\[\d+\]=(?:_v\d+|_m\d+_\d+);")


def macro_arguments(text: str, name: str) -> list[str]:
    """The top-level arguments of every `name(...)` call in `text`."""
    calls = []
    start = 0
    while True:
        i = text.find(name + "(", start)
        if i < 0:
            return calls
        start = i + len(name) + 1
        depth, args, j = 1, [""], start
        while depth:
            ch = text[j]
            if ch in "([{":
                depth += 1
            elif ch in ")]}":
                depth -= 1
                if depth == 0:
                    break
            if ch == "," and depth == 1:
                args.append("")
                j += 1
                continue
            args[-1] += ch
            j += 1
        calls.append(args)


def report_fields(report: str) -> dict[str, str]:
    """Split a NAN_TRAP line into its fields (pc, op, dst, vbl, in, out)."""
    m = re.fullmatch(r"NAN_TRAP pc=(0x[0-9a-f]+) op=(\S+) dst=(\S+) vbl=(\d+) "
                     r"in=\[([^\]]*)\] out=\[([^\]]*)\]", report)
    if not m:
        raise AssertionError(f"unparsable NAN_TRAP report: {report!r}")
    keys = ("pc", "op", "dst", "vbl", "in", "out")
    fields = dict(zip(keys, m.groups(), strict=True))
    fields["in"] = [v for v in fields["in"].split(",") if v]
    fields["out"] = [v for v in fields["out"].split(",") if v]
    return fields


def matrix_word(sub, vt, vs, vd, size):
    """A VFPU1 matrix-form word: opcode 0x3C, sub = form, size in bits 7 and 15."""
    size_bits = {1: 0, 2: 1 << 7, 3: 1 << 15, 4: (1 << 7) | (1 << 15)}[size]
    return (0x3C << 26) | (sub << 23) | (vt << 16) | (vs << 8) | vd | size_bits


def lane_stores(lanes, prefix):
    """C that loads one environment word per lane, in operand order."""
    return " ".join(f's->v[{idx}] = sr_bits(getenv("{prefix}{i}"));'
                    for i, idx in enumerate(lanes))


def lane_reads(dest):
    """C that prints each result lane's bits and whether it is non-finite."""
    return " ".join(
        f'{{ uint32_t b = sr_bits_out(&s->v[{idx}]);'
        f' printf("OUT{i}=0x%08x nonfinite=%d\\n", b,'
        f' (int)((b & 0x7f800000u) == 0x7f800000u)); }}'
        for i, idx in enumerate(dest))


# Operand words as raw IEEE-754 hex, so no fixture needs a host float literal.
FLT_MAX_BITS = "7f7fffff"   # the largest finite float; times 2 is +Inf
ONE_BITS = "3f800000"
TWO_BITS = "40000000"
NAN_BITS = "7fc00000"


def as_reported(bits):
    """How the reporter renders one finite word: the guest sees %.9g."""
    value = struct.unpack("<f", struct.pack("<I", int(bits, 16)))[0]
    return f"{value:.9g}"


def overflowing_env(matrix_lanes, vector_lanes):
    """All operands finite, one of them so large that the product overflows."""
    env = {f"M{i}": (FLT_MAX_BITS if i == 0 else ONE_BITS)
           for i in range(len(matrix_lanes))}
    env.update({f"V{i}": (TWO_BITS if i == 0 else ONE_BITS)
                for i in range(len(vector_lanes))})
    return env


def nan_vector_env(matrix_lanes, vector_lanes):
    """All operands finite except the first vector lane, which is NaN."""
    env = {f"M{i}": ONE_BITS for i in range(len(matrix_lanes))}
    env.update({f"V{i}": (NAN_BITS if i == 0 else ONE_BITS)
                for i in range(len(vector_lanes))})
    return env


def all_ones_env(matrix_lanes, vector_lanes):
    env = {f"M{i}": ONE_BITS for i in range(len(matrix_lanes))}
    env.update({f"V{i}": ONE_BITS for i in range(len(vector_lanes))})
    return env


def vtfm_layout(vt, vs, vd, size):
    """(matrix, vector, destination) lane indices, as the generator indexes them."""
    matrix = tuple(codegen.mreg_index(vs, size, i, k)
                   for i in range(size) for k in range(size))
    vector = tuple(codegen.vreg_indices(vt, size)[:size])
    return matrix, vector, tuple(codegen.vreg_indices(vd, size)[:size])


def vmmul_layout(vs, vt, vd, size):
    """(S, T, destination) lane indices: both matrices and the result, in order."""
    s_lanes = tuple(codegen.mreg_index(vs, size, b, a)
                    for a in range(size) for b in range(size))
    t_lanes = tuple(codegen.mreg_index(vt, size, a, b)
                    for a in range(size) for b in range(size))
    dest = tuple(codegen.mreg_index(vd, size, a, b)
                 for a in range(size) for b in range(size))
    return s_lanes, t_lanes, dest


class TestEmissionShape(unittest.TestCase):
    """What the generator emits, with and without the option."""

    # (label, producer, guest PC, mnemonic, expected fragments)
    CASES = [
        ("add.s", lambda: codegen.effect(0x00001234, cop1(0x10, 5, 6, 7, 0x00))[0],
         0x00001234, "add.s", ['SR_NAN_TRAP_F(0x00001234u,"add.s",7u,s->f[7],_a,_b)']),
        ("sub.s", lambda: codegen.effect(0x00001234, cop1(0x10, 5, 6, 7, 0x01))[0],
         0x00001234, "sub.s", ['SR_NAN_TRAP_F(0x00001234u,"sub.s",7u,s->f[7],_a,_b)']),
        ("mul.s", lambda: codegen.effect(0x00001234, cop1(0x10, 5, 6, 7, 0x02))[0],
         0x00001234, "mul.s", ['SR_NAN_TRAP_F(0x00001234u,"mul.s",7u,s->f[7],_a,_b)']),
        ("div.s", lambda: codegen.effect(0x00001234, cop1(0x10, 5, 6, 7, 0x03))[0],
         0x00001234, "div.s", ['SR_NAN_TRAP_F(0x00001234u,"div.s",7u,s->f[7],_a,_b)']),
        ("sqrt.s", lambda: codegen.effect(0x00005678, cop1(0x10, 0, 3, 3, 0x04))[0],
         0x00005678, "sqrt.s", ['SR_NAN_TRAP_F(0x00005678u,"sqrt.s",3u,s->f[3],_a)']),
        ("vdiv.s", lambda: codegen.effect(0x0000ABCD,
                                          vfpu_lane(0x18, 7, 1, 2, 3, 1))[0],
         0x0000ABCD, "vdiv.s", ['SR_NAN_TRAP_V2(0x0000abcdu,"vdiv.s",3u,_d,1,_s,1,_t,1)']),
        ("vadd.s", lambda: codegen.effect(0x0000ABCD,
                                          vfpu_lane(0x18, 0, 1, 2, 3, 2))[0],
         0x0000ABCD, "vadd.s", ['SR_NAN_TRAP_V2(0x0000abcdu,"vadd.s",3u,_d,2,_s,2,_t,2)']),
        ("vrcp.s", lambda: codegen.effect(0x0000BEEF,
                                          ((0x34 << 26) | (0 << 21) | (16 << 16)
                                           | (2 << 8) | 3))[0],
         0x0000BEEF, "vrcp.s", ['SR_NAN_TRAP_V(0x0000beefu,"vrcp.s",3u,_d,1,_s,1)']),
        ("vscl.s", lambda: codegen.effect(0x0000BEEF,
                                          vfpu_lane(0x19, 2, 1, 2, 3, 2))[0],
         0x0000BEEF, "vscl.s", ['SR_NAN_TRAP_V2(0x0000beefu,"vscl.s",3u,_d,2,_s,2,(&_sc),1)']),
    ]

    def test_option_off_emits_no_check_at_all(self):
        for label, produce, _pc, _op, _want in self.CASES:
            with self.subTest(op=label):
                text = produce()
                self.assertNotIn("SR_NAN_TRAP", text)

    def test_every_check_matches_its_macro_arity(self):
        """SR_NAN_TRAP_V takes 7 arguments and SR_NAN_TRAP_V2 takes 9.

        A 7-argument SR_NAN_TRAP_V2 compiled nowhere in the unit fixtures but
        failed every one-source VFPU form in a real title build.
        """
        arity = {"SR_NAN_TRAP_V": 7, "SR_NAN_TRAP_V2": 9}
        with nan_trap_codegen_state():
            for label, produce, _pc, _op, _want in self.CASES:
                text = produce()
                for name, want in arity.items():
                    start = 0
                    while True:
                        i = text.find(name + "(", start)
                        if i < 0:
                            break
                        start = i + len(name) + 1
                        depth, args, j = 1, 1, start
                        while depth:
                            ch = text[j]
                            if ch in "([{":
                                depth += 1
                            elif ch in ")]}":
                                depth -= 1
                            elif ch == "," and depth == 1:
                                args += 1
                            j += 1
                        with self.subTest(op=label, macro=name):
                            self.assertEqual(args, want, text[i:j])

    def test_option_on_names_pc_op_and_destination(self):
        with nan_trap_codegen_state():
            for label, produce, _pc, _op, want in self.CASES:
                with self.subTest(op=label):
                    text = produce()
                    for fragment in want:
                        self.assertIn(fragment, text)

    def test_trapped_form_is_the_historical_form_plus_only_a_check(self):
        # Every form except the single-source ones (which need the operand
        # sampled into a local first) is exactly the historical emission with one
        # check statement appended. Deleting the check restores it verbatim.
        check = re.compile(r" SR_NAN_TRAP_[A-Z0-9_]*\([^;]*\);")
        for label, produce, _pc, _op, _want in self.CASES:
            if label == "sqrt.s":
                continue
            with self.subTest(op=label):
                off_text = produce()
                with nan_trap_codegen_state():
                    on_text = produce()
                self.assertEqual(check.sub("", on_text), off_text)

    def test_vmmul_gathers_scattered_operands(self):
        # vmmul reads raw v[] elements, so the check materialises them first.
        word = ((0x3C << 26) | (0 << 23) | (0 << 21) | (4 << 16) | (2 << 8) | 4
                | (1 << 7) | (1 << 15))
        with nan_trap_codegen_state():
            text = codegen.vfpu_effect(0x00001111, word)[0]
        self.assertIn('SR_NAN_TRAP_V(0x00001111u,"vmmul",4u,_ntout,16,_ntin,32)', text)
        self.assertIn("float _ntin[32], _ntout[16];", text)
        codegen.NAN_TRAP = False
        self.assertNotIn("SR_NAN_TRAP", codegen.vfpu_effect(0x00001111, word)[0])
        codegen.NAN_TRAP = False

    def _matrix_word(self, sub, vt, vs, vd, size):
        size_bits = {1: 0, 2: 1 << 7, 3: 1 << 15, 4: (1 << 7) | (1 << 15)}[size]
        return ((0x3C << 26) | (sub << 23) | (vt << 16) | (vs << 8) | vd | size_bits)

    def _v_elements(self, text):
        """Every s->v[N] element the statement reads or writes, in order."""
        return [int(i) for i in re.findall(r"s->v\[(\d+)\]", text)]

    def test_vtfm_reports_the_matrix_lanes_and_the_vector_lanes(self):
        # vtfm2/3/4 (vhtfm shares the encoding). The vector lanes are consumed
        # exactly as much as the matrix lanes, so they get their own gathered
        # array and their own macro argument: a vtfm whose vector lane already
        # carried a NaN used to be reported as the origin of it, with the vector
        # nowhere in the record.
        for sub, size in ((1, 2), (2, 3), (3, 4)):
            with self.subTest(vtfm=f"vtfm{sub + 1}"):
                word = self._matrix_word(sub, 4, 0, 8, size)
                with nan_trap_codegen_state():
                    text = codegen.vfpu_effect(0x00001111, word)[0]
                side = sub + 1
                tn = min(codegen.vec_size(word), side)
                self.assertIn(f"float _ntin[{side * side}], _ntout[{side}], _ntin2[{tn}];", text)
                self.assertIn(
                    f'SR_NAN_TRAP_V2(0x00001111u,"vtfm",8u,_ntout,{side},'
                    f'_ntin,{side * side},_ntin2,{tn});', text)

    def test_matrix_forms_gather_exactly_the_elements_they_multiply(self):
        # The property, not one encoding: every v[] element the matrix form reads
        # is in the operand list, and no element is in the list without being
        # read. A partial gather turns a propagation into a false origin.
        checked = 0
        with nan_trap_codegen_state():
            for sub in (0, 1, 2, 3):
                for size in (1, 2, 3, 4):
                    for vt, vs, vd in ((4, 0, 8), (0, 2, 1), (36, 4, 8), (5, 5, 9)):
                        word = self._matrix_word(sub, vt, vs, vd, size)
                        text = codegen.vfpu_effect(0x00001111, word)[0]
                        if "SR_NAN_TRAP" not in text:
                            continue
                        checked += 1
                        block = GATHER_BLOCK.search(text)
                        self.assertIsNotNone(block, text)
                        gathered = self._v_elements(block.group(0))
                        computed = self._v_elements(MATRIX_WRITES.sub("", text))
                        with self.subTest(word=f"0x{word:08x}"):
                            self.assertEqual(sorted(set(gathered)), sorted(set(computed)))
        self.assertGreaterEqual(checked, 32, "the matrix sweep found too few forms")

    def test_constant_broadcasts_carry_no_check(self):
        # A documented boundary, asserted so it cannot drift silently: a
        # constant has no operand to be non-finite relative to, and the integer
        # pack/unpack forms write integer words rather than a computed float.
        constants = [
            ((0x37 << 26) | (3 << 24) | (1 << 16) | 0x4000),      # viim
            ((0x34 << 26) | (3 << 21) | (1 << 16) | (0 << 8) | 3),  # vcst
            ((0x34 << 26) | (0 << 21) | (6 << 16) | (0 << 8) | 3),  # vzero
            ((0x34 << 26) | (0 << 21) | (3 << 16) | (0 << 8) | 3),  # vidt
            ((0x34 << 26) | (1 << 21) | (28 << 16) | (0 << 8) | 3),  # vi2uc
            ((0x34 << 26) | (16 << 21) | (0 << 16) | (0 << 8) | 3),  # vf2in
        ]
        with nan_trap_codegen_state():
            for word in constants:
                with self.subTest(word=f"0x{word:08x}"):
                    self.assertNotIn("SR_NAN_TRAP", codegen.vfpu_effect(0x00002222, word)[0])


class TestGeneratedFixtureIsUnchangedWhenOff(unittest.TestCase):
    """The zero-cost claim, proved over a real generated guest."""

    @classmethod
    def setUpClass(cls):
        if not CC:
            raise unittest.SkipTest("no C compiler on PATH")
        cls.tmp = Path(tempfile.mkdtemp(prefix="nan_trap_fixture_"))
        fixture = cls.tmp / "fixture"
        subprocess.run([PY, str(COSIM_GENERATOR), "generate", "--out-dir", str(fixture)],
                       check=True, capture_output=True, text=True, cwd=ROOT)
        cls.dirs = {}
        for label, extra in (("off", []), ("on", ["--nan-trap"])):
            out = cls.tmp / label
            out.mkdir()
            subprocess.run([PY, str(ROOT / "tools" / "codegen.py"),
                            str(fixture / "guest.prx"), str(out / "guest_recomp.c"),
                            f"--base={COSIM_BASE}", "--funcs-per-chunk=200", *extra],
                           check=True, capture_output=True, text=True, cwd=ROOT)
            cls.dirs[label] = out

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_only_the_trapped_run_mentions_the_trap(self):
        chunk = (self.dirs["off"] / "guest_recomp_0.c").read_text(encoding="ascii")
        self.assertNotIn("SR_NAN_TRAP", chunk)
        trapped = (self.dirs["on"] / "guest_recomp_0.c").read_text(encoding="ascii")
        self.assertIn("SR_NAN_TRAP", trapped)
        self.assertNotEqual(chunk, trapped, "--nan-trap must change the emission")

    def test_trap_free_output_is_reproducible(self):
        # Same inputs, same bytes: the untrapped run is a stable baseline a
        # reviewer can diff against a pre-trap build.
        again = self.tmp / "again"
        again.mkdir()
        subprocess.run([PY, str(ROOT / "tools" / "codegen.py"),
                        str(self.tmp / "fixture" / "guest.prx"),
                        str(again / "guest_recomp.c"),
                        f"--base={COSIM_BASE}", "--funcs-per-chunk=200"],
                       check=True, capture_output=True, text=True, cwd=ROOT)
        for name in ("guest_recomp.c", "guest_recomp_0.c", "guest_recomp_funcs.h"):
            with self.subTest(path=name):
                self.assertEqual((self.dirs["off"] / name).read_bytes(),
                                 (again / name).read_bytes())

    def test_trap_free_headers_are_shared_by_both_runs(self):
        # The registration file and the declarations never depend on the option.
        for name in ("guest_recomp.c", "guest_recomp_funcs.h"):
            with self.subTest(path=name):
                self.assertEqual((self.dirs["off"] / name).read_bytes(),
                                 (self.dirs["on"] / name).read_bytes())

    def test_untrapped_object_is_byte_identical_with_and_without_the_define(self):
        # The C half on its own must also cost nothing: compiling the same
        # untrapped chunk with -DSR_NAN_TRAP (which turns every SR_NAN_TRAP_*
        # macro into ((void)0)) must produce the same machine code.
        chunk = (self.dirs["off"] / "guest_recomp_0.c").read_text(encoding="ascii")
        objects = {}
        for label, flag in (("plain", []), ("defined", ["-DSR_NAN_TRAP"])):
            obj = self.tmp / f"chunk_{label}.o"
            result = subprocess.run(
                [CC, "-std=c11", "-O1", "-w", "-I", os.fspath(RT), "-I",
                 os.fspath(ROOT / "src" / "core"), "-I", os.fspath(self.dirs["off"]),
                 *flag, "-x", "c", "-c", "-", "-o", os.fspath(obj)],
                input=chunk, capture_output=True, text=True, cwd=ROOT)
            self.assertEqual(result.returncode, 0, result.stderr)
            objects[label] = obj.read_bytes()
        self.assertEqual(objects["plain"], objects["defined"])
        self.assertGreater(len(objects["plain"]), 0)


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestMatrixFormsCostNothingWhenOff(unittest.TestCase):
    """The zero-cost claim for the matrix forms, which gather operands.

    The generated guest fixture above proves it for whatever that fixture
    happens to contain; the matrix forms own the only arrays the option adds
    (float _ntin[]/ _ntout[]/ _ntin2[] on the stack), so they are proved here
    directly: the untrapped statement is the trapped one minus its gather block,
    and the machine code compiled from it does not move when the define is set.
    """

    FORMS = [("vmmul", matrix_word(0, 4, 0, 8, 4)),
             ("vtfm2", matrix_word(1, 4, 0, 8, 2)),
             ("vtfm3", matrix_word(2, 4, 0, 8, 3)),
             ("vtfm4", matrix_word(3, 4, 0, 8, 4))]

    def statements(self, trapped: bool):
        previous = codegen.NAN_TRAP
        codegen.NAN_TRAP = trapped
        try:
            return [codegen.effect(0x00001111, word)[0] for _name, word in self.FORMS]
        finally:
            codegen.NAN_TRAP = previous

    def test_every_matrix_form_gathers_operands_when_the_option_is_on(self):
        for (name, _word), text in zip(self.FORMS, self.statements(True), strict=True):
            with self.subTest(form=name):
                self.assertIn("SR_NAN_TRAP", text)
                self.assertIsNotNone(GATHER_BLOCK.search(text), text)

    def test_the_untrapped_statement_is_the_trapped_one_without_the_block(self):
        for (name, _word), on_text, off_text in zip(self.FORMS, self.statements(True),
                                                   self.statements(False), strict=True):
            with self.subTest(form=name):
                self.assertEqual(GATHER_BLOCK.sub("", on_text), off_text)
                self.assertNotIn("SR_NAN_TRAP", off_text)

    def test_the_untrapped_object_is_byte_identical_with_and_without_the_define(self):
        body = "\n".join(f"void form_{i}(CpuState *s) {{ {text} }}"
                         for i, text in enumerate(self.statements(False)))
        objects = {}
        for label, flag in (("plain", []), ("defined", ["-DSR_NAN_TRAP"])):
            obj = Path(tempfile.mkdtemp(prefix="nan_trap_matrix_")) / f"{label}.o"
            self.addCleanup(obj.unlink, True)
            result = subprocess.run(
                [CC, "-std=c11", "-O1", "-w", "-I", os.fspath(RT), "-I",
                 os.fspath(ROOT / "src" / "core"), *flag, "-x", "c", "-c", "-",
                 "-o", os.fspath(obj)],
                input='#include "recomp.h"\n' + body + "\n",
                capture_output=True, text=True, cwd=ROOT)
            self.assertEqual(result.returncode, 0, result.stderr)
            objects[label] = obj.read_bytes()
        self.assertEqual(objects["plain"], objects["defined"])
        self.assertGreater(len(objects["plain"]), 0)


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestEveryTrappedVfpuFormCompiles(unittest.TestCase):
    """Compile, with -DSR_NAN_TRAP, every VFPU statement the generator traps.

    String-shape tests passed while a real title build failed twice: once on a
    7-argument SR_NAN_TRAP_V2 and once on a scalar handed to a const float *
    parameter (vrot). This sweeps the VFPU encoding space through the real
    generator and asks the compiler.
    """

    def sweep(self) -> dict[str, str]:
        """Every distinct trapped VFPU statement the generator emits, by word."""
        snippets = {}
        with nan_trap_codegen_state():
            size_bits = {1: 0, 2: 1 << 7, 3: 1 << 15, 4: (1 << 7) | (1 << 15)}
            for op in range(0x18, 0x40):
                for field in range(1 << 10):  # bits 16..25: function select and vt
                    for size in (1, 2, 3, 4):
                        word = ((op << 26) | (field << 16) | (2 << 8) | 3
                                | size_bits[size])
                        try:
                            text = codegen.effect(0x00001000, word)[0]
                        except Exception:
                            continue
                        if text and "SR_NAN_TRAP" in text and text not in snippets:
                            snippets[text] = f"0x{word:08x}"
        return snippets

    def test_all_trapped_vfpu_statements_compile(self):
        snippets = self.sweep()
        self.assertGreater(len(snippets), 20, "the sweep found too few trapped forms")
        body = ['#include "recomp.h"', "#include <math.h>"]
        for i, (text, word) in enumerate(snippets.items()):
            body.append(f"/* {word} */ void nan_trap_form_{i}(CpuState *s) {{ {text} }}")
        tmp = Path(tempfile.mkdtemp(prefix="nan_trap_sweep_"))
        self.addCleanup(shutil.rmtree, tmp, True)
        src = tmp / "sweep.c"
        src.write_text("\n".join(body) + "\n", encoding="ascii")
        result = subprocess.run(
            [CC, "-std=c11", "-fsyntax-only", "-Werror=incompatible-pointer-types",
             "-Werror=int-conversion", "-Isrc/rt", "-Isrc/core", "-DSR_NAN_TRAP",
             os.fspath(src)],
            capture_output=True, text=True, cwd=ROOT)
        self.assertEqual(result.returncode, 0, result.stderr[-3000:])

    def test_every_gathered_operand_count_matches_the_array_it_fills(self):
        # The matrix forms gather their operands into arrays; a count argument
        # that disagreed with the array would read past it in the report (or
        # leave a lane unreported), and nothing else in the sweep would say so.
        forms = 0
        for text, word in self.sweep().items():
            block = GATHER_BLOCK.search(text)
            if not block:
                continue
            forms += 1
            declaration = block.group(0).split(";")[0]
            arrays = dict(re.findall(r"(_nt(?:in|out)2?)\[(\d+)\]", declaration))
            calls = macro_arguments(text, "SR_NAN_TRAP_V") + macro_arguments(
                text, "SR_NAN_TRAP_V2")
            self.assertEqual(len(calls), 1, word)
            args = calls[0]
            self.assertEqual(int(args[4]), int(arrays["_ntout"]), word)
            self.assertEqual(int(args[6]), int(arrays["_ntin"]), word)
            if len(args) == 9:
                self.assertEqual(int(args[8]), int(arrays["_ntin2"]), word)
        self.assertGreater(forms, 8, "the sweep found too few matrix forms")

    def test_the_sweep_reaches_both_matrix_opcode_families(self):
        names = set()
        for text in self.sweep():
            for args in (macro_arguments(text, "SR_NAN_TRAP_V")
                         + macro_arguments(text, "SR_NAN_TRAP_V2")):
                names.add(args[1])
        self.assertIn('"vmmul"', names)
        self.assertIn('"vtfm"', names)


WATCHPOINT_STUB = """
#include "watchpoints_file.h"
int sr_parse_watchpoints_file(const char *path, SrWatchpointEntry *out, int out_cap,
                              char *errbuf, size_t errbuf_size) {
    (void)path; (void)out; (void)out_cap; (void)errbuf; (void)errbuf_size;
    return 0;
}
"""

# The guest VBLANK count the reporter prints as vbl=. The real definition is
# hle.c's (the mirror it hands to ge_set_frame, the same number the
# SR_GE_TRANSITION_TRACE stamps as its frame); here it comes from the
# environment so a test can prove the field carries the runtime's counter
# instead of a constant baked into the reporter.
VBL_STUB = """
uint32_t sr_audio_vbl(void) {
    const char *text = getenv("SR_VBL");
    return text && text[0] ? (uint32_t)strtoul(text, NULL, 10) : 0u;
}
"""


@unittest.skipUnless(CC, "no C compiler on PATH")
class TrapHarness:
    """Builds one C fixture: real generated statement + real reporter."""

    tmp: Path

    def compile(self, name: str, body: str, main: str, extra_sources=()) -> Path:
        tmp = Path(tempfile.mkdtemp(prefix="nan_trap_"))
        self.addCleanup(shutil.rmtree, tmp, True)
        source = tmp / f"{name}.c"
        source.write_text(
            '#include "recomp.h"\n#include <stdio.h>\n#include <string.h>\n'
            "#include <stdlib.h>\n"
            + VBL_STUB + body + "\n" + main + "\n",
            encoding="ascii")
        stub = tmp / "stub.c"
        stub.write_text(WATCHPOINT_STUB, encoding="ascii")
        exe = tmp / (name + (".exe" if os.name == "nt" else ""))
        result = subprocess.run(
            [CC, "-std=c11", "-O1", "-Isrc/rt", "-Isrc/core", "-DSR_NAN_TRAP",
             "-o", os.fspath(exe), os.fspath(source), os.fspath(stub),
             os.fspath(RT / "debug.c"), *[os.fspath(RT / s) for s in extra_sources],
             "-lm"],
            capture_output=True, text=True, cwd=ROOT)
        if result.returncode != 0:
            self.fail(f"{name} did not compile:\n{result.stderr}")
        return exe

    @staticmethod
    def run_exe(exe: Path, env: dict[str, str]) -> tuple[subprocess.CompletedProcess, list[str]]:
        environ = dict(os.environ)
        environ.setdefault("SR_VBL", "4711")
        environ.update(env)
        result = subprocess.run([os.fspath(exe)], capture_output=True, text=True,
                                env=environ, cwd=ROOT)
        reports = [line for line in result.stderr.splitlines() if line.startswith("NAN_TRAP pc=")]
        return result, reports


class TestScalarFpuTrap(TrapHarness, unittest.TestCase):
    """A real codegen emission, compiled against the real reporter."""

    PC = 0x00001234

    def setUp(self):
        with nan_trap_codegen_state():
            self.statement = codegen.effect(
                self.PC, cop1(0x10, 5, 6, 7, 0x03))[0]   # div.s f7, f6, f5
        self.assertIn("SR_NAN_TRAP_F", self.statement)
        self.exe = self.compile(
            "div_s",
            f"static void body(CpuState *s) {{ {self.statement} }}",
            """
static void set_f(CpuState *s, int reg, uint32_t bits) { s->fi[reg] = bits; }
int main(void) {
    CpuState st; CpuState *s = &st;
    memset(s, 0, sizeof *s);
    set_f(s, 6, (uint32_t)strtoul(getenv("NUM"), NULL, 16));
    set_f(s, 5, (uint32_t)strtoul(getenv("DEN"), NULL, 16));
    body(s);
    printf("RESULT=0x%08x\\n", s->fi[7]);
    return 0;
}
""")

    def test_finite_operands_producing_nan_are_reported(self):
        result, reports = self.run_exe(self.exe, {"NUM": "00000000", "DEN": "00000000"})
        self.assertEqual(result.stdout.strip(), "RESULT=0xffc00000")
        self.assertEqual(len(reports), 1, result.stderr)
        self.assertEqual(reports[0],
                         "NAN_TRAP pc=0x00001234 op=div.s dst=f7 vbl=4711 in=[0,0] out=[nan]")

    def test_finite_operands_producing_inf_are_reported(self):
        _result, reports = self.run_exe(self.exe, {"NUM": "3f800000", "DEN": "00000000"})
        self.assertEqual(reports, ["NAN_TRAP pc=0x00001234 op=div.s dst=f7 vbl=4711 in=[1,0] out=[inf]"])

    def test_finite_result_is_silent(self):
        result, reports = self.run_exe(self.exe, {"NUM": "3f800000", "DEN": "40000000"})
        self.assertEqual(result.stdout.strip(), "RESULT=0x3f000000")
        self.assertEqual(reports, [])

    def test_propagating_an_existing_nan_is_not_reported(self):
        # The trap names the ORIGIN. A sequence that only echoes an existing NaN
        # must stay silent, or the maintainer would be sent to the last
        # instruction in a long chain instead of the one that made it.
        result, reports = self.run_exe(self.exe, {"NUM": "7fc00000", "DEN": "3f800000"})
        self.assertEqual(result.stdout.strip(), "RESULT=0x7fc00000")
        self.assertEqual(reports, [])

    def test_propagating_an_existing_inf_is_not_reported(self):
        _result, reports = self.run_exe(self.exe, {"NUM": "7f800000", "DEN": "3f800000"})
        self.assertEqual(reports, [])

    def test_limit_env_bounds_the_report(self):
        # Four NaN origins, limit 2: exactly two reports plus the stop notice.
        exe = self.compile(
            "div_loop",
            f"static void body(CpuState *s) {{ {self.statement} }}",
            """
int main(void) {
    CpuState st; CpuState *s = &st;
    for (int i = 0; i < 4; i++) {
        memset(s, 0, sizeof *s);
        body(s);
    }
    return 0;
}
""")
        _result, reports = self.run_exe(exe, {"NUM": "00000000", "DEN": "00000000",
                                          "SR_NAN_TRAP_LIMIT": "2"})
        self.assertEqual(len(reports), 2, "limit must bound the reports")
        _result, reports = self.run_exe(exe, {"NUM": "00000000", "DEN": "00000000",
                                          "SR_NAN_TRAP_LIMIT": "0"})
        self.assertEqual(reports, [], "limit 0 silences the trap entirely")

    def test_default_limit_is_twenty(self):
        exe = self.compile(
            "div_many",
            f"static void body(CpuState *s) {{ {self.statement} }}",
            """
int main(void) {
    CpuState st; CpuState *s = &st;
    for (int i = 0; i < 25; i++) {
        memset(s, 0, sizeof *s);
        body(s);
    }
    return 0;
}
""")
        _result, reports = self.run_exe(exe, {"NUM": "00000000", "DEN": "00000000"})
        self.assertEqual(len(reports), 20, "SR_NAN_TRAP_DEFAULT_LIMIT is 20")


VFPU_STUBS = """
/* Bit-exact float plumbing for the fixtures. A float read back through an
 * unsigned* is a strict-aliasing violation the optimiser is free to exploit, so
 * every crossing goes through memcpy exactly as the production code does. */
static float sr_bits(const char *text) {
    uint32_t bits = (uint32_t)strtoul(text, NULL, 16);
    float value;
    memcpy(&value, &bits, 4);
    return value;
}
static uint32_t sr_bits_out(const float *value) {
    uint32_t bits;
    memcpy(&bits, value, 4);
    return bits;
}

/* Identity source/destination prefix handling: the trapped instruction is
 * executed with vfpuCtrl all zero, so the real prefix kernels would copy the
 * lanes through unchanged. */
void sr_vread(float *r, const CpuState *s, const uint8_t *idx, int n, uint32_t prefix) {
    (void)prefix;
    for (int i = 0; i < n; i++) r[i] = s->v[idx[i]];
}
void sr_vwrite(CpuState *s, const uint8_t *idx, float *d, int n, uint32_t dprefix) {
    (void)dprefix;
    for (int i = 0; i < n; i++) s->v[idx[i]] = d[i];
}
float sr_vfpu_rcp(float x) { return x == 0.0f ? 1.0f / x : 1.0f / x; }
float sr_vfpu_rsqrt(float x) { return 1.0f / x; }
float sr_vfpu_sqrt(float x) { return (float)__builtin_sqrt((double)x); }
float sr_vfpu_asin(float x) { return x; }
float sr_vfpu_log2(float x) { return x; }
float sr_vfpu_sin(float x) { return x; }
float sr_vfpu_cos(float x) { return x; }
float sr_vfpu_exp2(float x) { return x; }
"""


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestVfpuLaneTrap(TrapHarness, unittest.TestCase):
    """A real VFPU lane emission from codegen.py, with the real reporter."""

    PC = 0x0000ABCD

    def setUp(self):
        self.word = vfpu_lane(0x18, 7, 1, 2, 3, 1)          # vdiv.s v3, v2, v1
        with nan_trap_codegen_state():
            self.statement = codegen.effect(self.PC, self.word)[0]
        self.assertIn("SR_NAN_TRAP_V2", self.statement)
        self.s_index = codegen.vreg_indices(2, 1)[0]
        self.t_index = codegen.vreg_indices(1, 1)[0]
        self.d_index = codegen.vreg_indices(3, 1)[0]
        self.exe = self.compile(
            "vdiv_s",
            f"{VFPU_STUBS}\nstatic void body(CpuState *s) {{ {self.statement} }}",
            f"""
int main(void) {{
    CpuState st; CpuState *s = &st;
    memset(s, 0, sizeof *s);
    s->v[{self.s_index}] = sr_bits(getenv("NUM"));
    s->v[{self.t_index}] = sr_bits(getenv("DEN"));
    body(s);
    printf("RESULT=0x%08x\\n", sr_bits_out(&s->v[{self.d_index}]));
    return 0;
}}
""")

    def test_finite_operands_producing_nan_are_reported(self):
        result, reports = self.run_exe(self.exe, {"NUM": "00000000", "DEN": "00000000"})
        self.assertEqual(len(reports), 1, result.stderr)
        self.assertEqual(reports[0],
                         "NAN_TRAP pc=0x0000abcd op=vdiv.s dst=v3 vbl=4711 in=[0,0] out=[nan]")

    def test_finite_result_is_silent(self):
        result, reports = self.run_exe(self.exe, {"NUM": "3f800000", "DEN": "40000000"})
        self.assertEqual(result.stdout.strip(), "RESULT=0x3f000000")
        self.assertEqual(reports, [])

    def test_propagating_an_existing_nan_is_not_reported(self):
        _result, reports = self.run_exe(self.exe, {"NUM": "7fc00000", "DEN": "3f800000"})
        self.assertEqual(reports, [])


# Host symbols the single-instruction VFPU interpreter reaches that these
# fixtures never exercise. Their bodies are inert on purpose: a fixture that
# accidentally reached one would fail loudly rather than quietly disagree.
INTERP_STUBS = VFPU_STUBS + """
uint8_t *g_mem;
int g_hle_depth;
int g_sr_heap_watch;
CpuState *s_cpu;
void sr_oor(uint32_t a, uint32_t v, int store) { (void)a; (void)v; (void)store; }
void sr_heap_note_write(uint32_t a, uint32_t w, uint32_t v, uint32_t pc) {
    (void)a; (void)w; (void)v; (void)pc;
}
uint32_t sched_current_uid(void) { return 0; }
uint32_t sr_get_ge_status(void) { return 0; }
int sr_cpu_lle_enabled(void) { return 0; }
unsigned sr_cpu_data_access_fault(const CpuState *s, uint32_t addr, unsigned width,
                                  int store) {
    (void)s; (void)addr; (void)width; (void)store; return 0;
}
int sr_cpu_raise_data_fault(CpuState *s, unsigned code, uint32_t addr, uint32_t pc) {
    (void)s; (void)code; (void)addr; (void)pc; return 0;
}
"""


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestInterpreterTrap(TrapHarness, unittest.TestCase):
    """The AOT-gap fallback path: the real src/rt/vfpu_interp.c."""

    PC = 0x00005678

    def setUp(self):
        self.word = vfpu_lane(0x18, 7, 1, 2, 3, 1)          # vdiv.s v3, v2, v1
        self.s_index = codegen.vreg_indices(2, 1)[0]
        self.t_index = codegen.vreg_indices(1, 1)[0]
        self.d_index = codegen.vreg_indices(3, 1)[0]
        self.exe = self.compile(
            "vfpu_interp",
            INTERP_STUBS,
            f"""
int main(void) {{
    CpuState st; CpuState *s = &st;
    memset(s, 0, sizeof *s);
    s->pc = 0x{self.PC:08x}u;
    s->v[{self.s_index}] = sr_bits(getenv("NUM"));
    s->v[{self.t_index}] = sr_bits(getenv("DEN"));
    int rc = sr_vfpu_interp(s, 0x{self.word:08x}u);
    printf("RC=%d RESULT=0x%08x\\n", rc, sr_bits_out(&s->v[{self.d_index}]));
    return 0;
}}
""",
            extra_sources=("vfpu_interp.c",))

    def test_finite_operands_producing_nan_are_reported(self):
        result, reports = self.run_exe(self.exe, {"NUM": "00000000", "DEN": "00000000"})
        self.assertEqual(result.stdout.strip(), "RC=1 RESULT=0xffc00000")
        self.assertEqual(reports, [f"NAN_TRAP pc=0x{self.PC:08x} op=vdiv.s dst=v3 vbl=4711 in=[0,0] out=[nan]"])

    def test_finite_result_is_silent(self):
        result, reports = self.run_exe(self.exe, {"NUM": "3f800000", "DEN": "40000000"})
        self.assertEqual(result.stdout.strip(), "RC=1 RESULT=0x3f000000")
        self.assertEqual(reports, [])

    def test_propagating_an_existing_nan_is_not_reported(self):
        _result, reports = self.run_exe(self.exe, {"NUM": "7fc00000", "DEN": "3f800000"})
        self.assertEqual(reports, [])


class TestInterpreterOneSourceTrap(TrapHarness, unittest.TestCase):
    """A one-source form through the interpreter reports its single input."""

    PC = 0x00005680

    def setUp(self):
        # vsqrt.s v3, v2: VV2Op (0x34), optype 22, scalar.
        self.word = (0x34 << 26) | (22 << 16) | (2 << 8) | 3
        self.s_index = codegen.vreg_indices(2, 1)[0]
        self.d_index = codegen.vreg_indices(3, 1)[0]
        self.exe = self.compile(
            "vfpu_interp_one_source",
            INTERP_STUBS,
            f"""
int main(void) {{
    CpuState st; CpuState *s = &st;
    memset(s, 0, sizeof *s);
    s->pc = 0x{self.PC:08x}u;
    s->v[{self.s_index}] = sr_bits(getenv("ARG"));
    int rc = sr_vfpu_interp(s, 0x{self.word:08x}u);
    printf("RC=%d RESULT=0x%08x\\n", rc, sr_bits_out(&s->v[{self.d_index}]));
    return 0;
}}
""",
            extra_sources=("vfpu_interp.c",))

    def test_finite_input_producing_nan_reports_one_input(self):
        _result, reports = self.run_exe(self.exe, {"ARG": "bf800000"})  # sqrt(-1)
        self.assertEqual(len(reports), 1, reports)
        self.assertTrue(reports[0].startswith(f"NAN_TRAP pc=0x{self.PC:08x} op=vtrig.s dst=v3 vbl=4711 in=[-1]"),
                        reports[0])
        self.assertTrue(reports[0].endswith("out=[nan]"), reports[0])

    def test_finite_result_is_silent(self):
        _result, reports = self.run_exe(self.exe, {"ARG": "40800000"})  # sqrt(4)
        self.assertEqual(reports, [])


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestVtfmTrap(TrapHarness, unittest.TestCase):
    """A vtfm emission compiled against the real reporter: every lane, and a frame.

    The blind spot this closes: the check used to gather the matrix lanes only, so
    a vtfm whose VECTOR lane already carried a NaN was reported as the
    instruction that made it -- a false origin pointing past the real one -- and
    the vector was nowhere in the record.
    """

    PC = 0x00002222

    def setUp(self):
        # vtfm3 m8, v0, v8: matrix 0, vector 4, result matrix 2.
        self.word = matrix_word(2, 4, 0, 8, 3)
        self.matrix, self.vector, self.dest = vtfm_layout(4, 0, 8, 3)
        with nan_trap_codegen_state():
            self.statement = codegen.effect(self.PC, self.word)[0]
        loads = f"{lane_stores(self.matrix, 'M')} {lane_stores(self.vector, 'V')}"
        self.generated = self.compile(
            "vtfm3",
            f"{VFPU_STUBS}\nstatic void body(CpuState *s) {{ {self.statement} }}",
            f"""
int main(void) {{
    CpuState st; CpuState *s = &st;
    memset(s, 0, sizeof *s);
    {loads}
    body(s);
    {lane_reads(self.dest)}
    return 0;
}}
""")
        self.interpreted = self.compile(
            "vtfm3_interp", INTERP_STUBS,
            f"""
int main(void) {{
    CpuState st; CpuState *s = &st;
    memset(s, 0, sizeof *s);
    s->pc = 0x{self.PC:08x}u;
    {loads}
    int rc = sr_vfpu_interp(s, 0x{self.word:08x}u);
    printf("RC=%d\\n", rc);
    {lane_reads(self.dest)}
    return 0;
}}
""",
            extra_sources=("vfpu_interp.c",))

    def test_finite_operands_report_the_matrix_and_the_vector_lanes(self):
        result, reports = self.run_exe(self.generated,
                                       overflowing_env(self.matrix, self.vector))
        self.assertEqual(len(reports), 1, result.stderr)
        fields = report_fields(reports[0])
        self.assertEqual(fields["pc"], f"0x{self.PC:08x}")
        self.assertEqual(fields["op"], "vtfm")
        self.assertEqual(fields["dst"], "v8")
        # The 9 matrix lanes first, then the 3 vector lanes: the vector is what
        # the check used to drop, and its lanes are the last three values.
        self.assertEqual(len(fields["in"]), 12, reports[0])
        self.assertEqual(fields["in"][:9],
                         [as_reported(FLT_MAX_BITS)] + ["1"] * 8, reports[0])
        self.assertEqual(fields["in"][9:], ["2", "1", "1"], reports[0])
        self.assertEqual(fields["out"], ["inf", "4", "4"], reports[0])

    def test_the_reported_frame_is_the_runtimes_vblank_counter(self):
        _result, reports = self.run_exe(
            self.generated,
            dict(overflowing_env(self.matrix, self.vector), SR_VBL="90210"))
        self.assertEqual(report_fields(reports[0])["vbl"], "90210", reports[0])

    def test_nan_vector_lane_is_not_reported_as_a_vtfm_origin(self):
        # The vector lane is an operand, so a NaN in it is propagation, not
        # origin: silence here is what leaves the real origin (the instruction
        # that made the lane NaN) as the one that reports.
        result, reports = self.run_exe(self.generated,
                                       nan_vector_env(self.matrix, self.vector))
        self.assertEqual(reports, [], result.stderr)
        self.assertIn("nonfinite=1", result.stdout, "the result is still NaN")

    def test_finite_result_is_silent(self):
        result, reports = self.run_exe(self.generated,
                                       all_ones_env(self.matrix, self.vector))
        self.assertEqual(reports, [], result.stderr)
        self.assertNotIn("nonfinite=1", result.stdout)

    def test_the_interpreter_reports_the_same_operands_for_the_same_word(self):
        # Both tiers must gather the same operand list, or a report would depend
        # on which tier the instruction happened to run in.
        env = overflowing_env(self.matrix, self.vector)
        _result, generated = self.run_exe(self.generated, env)
        _result, interpreted = self.run_exe(self.interpreted, env)
        self.assertEqual(interpreted, generated)

    def test_the_interpreter_also_stays_silent_for_a_nan_vector_lane(self):
        result, reports = self.run_exe(self.interpreted,
                                       nan_vector_env(self.matrix, self.vector))
        self.assertEqual(reports, [], result.stderr)
        self.assertIn("RC=1", result.stdout, "the interpreter still computed it")


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestVmmulTrap(TrapHarness, unittest.TestCase):
    """A vmmul emission: both matrices, every lane, in operand order."""

    PC = 0x00002333

    def setUp(self):
        # vmmul.4 p8, v0, v4: S = matrix 0, T = matrix 1, result = matrix 2.
        self.word = matrix_word(0, 4, 0, 8, 4)
        with nan_trap_codegen_state():
            self.statement = codegen.effect(self.PC, self.word)[0]
        self.assertIn('SR_NAN_TRAP_V(0x00002333u,"vmmul",8u,_ntout,16,_ntin,32)',
                      self.statement)
        self.s_lanes, self.t_lanes, self.dest = vmmul_layout(0, 4, 8, 4)
        self.exe = self.compile(
            "vmmul4",
            f"{VFPU_STUBS}\nstatic void body(CpuState *s) {{ {self.statement} }}",
            f"""
int main(void) {{
    CpuState st; CpuState *s = &st;
    memset(s, 0, sizeof *s);
    {lane_stores(self.s_lanes, 'S')}{lane_stores(self.t_lanes, 'T')}
    body(s);
    {lane_reads(self.dest)}
    return 0;
}}
""")

    def _env(self, s_bits, t_bits):
        env = {f"S{i}": s_bits for i in range(len(self.s_lanes))}
        env.update({f"T{i}": t_bits for i in range(len(self.t_lanes))})
        return env

    def test_both_matrices_are_reported_lane_for_lane(self):
        result, reports = self.run_exe(self.exe, self._env(ONE_BITS, FLT_MAX_BITS))
        self.assertEqual(len(reports), 1, result.stderr)
        fields = report_fields(reports[0])
        self.assertEqual((fields["op"], fields["dst"], fields["vbl"]),
                         ("vmmul", "v8", "4711"), reports[0])
        self.assertEqual(len(fields["in"]), 32, reports[0])
        self.assertEqual(fields["in"][:16], ["1"] * 16, reports[0])
        self.assertEqual(fields["in"][16:], [as_reported(FLT_MAX_BITS)] * 16, reports[0])
        # Every product row sums four T lanes, so every result overflows.
        self.assertEqual(fields["out"], ["inf"] * 16, reports[0])

    def test_a_nan_in_either_matrix_is_propagation_not_origin(self):
        env = self._env(ONE_BITS, ONE_BITS)
        env["T7"] = NAN_BITS
        result, reports = self.run_exe(self.exe, env)
        self.assertEqual(reports, [], result.stderr)
        self.assertEqual(result.stdout.count("nonfinite=1"), 4,
                         "one product column of four results carries the NaN")


if __name__ == "__main__":
    unittest.main()
