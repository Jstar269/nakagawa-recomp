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

Every C fixture below is generated into a temporary directory and links the real
src/rt/debug.c reporter; the only stubs are host symbols those fixtures never
reach (SDL ticks, GE status, the debug watchpoint file reader) and, for the
interpreter fixture, the runtime kernels the exercised instruction does not call.
"""

import os
import re
import shutil
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
         0x0000BEEF, "vrcp.s", ['SR_NAN_TRAP_V2(0x0000beefu,"vrcp.s",3u,_d,1,_s,1)']),
        ("vscl.s", lambda: codegen.effect(0x0000BEEF,
                                          vfpu_lane(0x19, 2, 1, 2, 3, 2))[0],
         0x0000BEEF, "vscl.s", ['SR_NAN_TRAP_V2(0x0000beefu,"vscl.s",3u,_d,2,_s,2,(&_sc),1)']),
    ]

    def test_option_off_emits_no_check_at_all(self):
        for label, produce, _pc, _op, _want in self.CASES:
            with self.subTest(op=label):
                text = produce()
                self.assertNotIn("SR_NAN_TRAP", text)

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


WATCHPOINT_STUB = """
#include "watchpoints_file.h"
int sr_parse_watchpoints_file(const char *path, SrWatchpointEntry *out, int out_cap,
                              char *errbuf, size_t errbuf_size) {
    (void)path; (void)out; (void)out_cap; (void)errbuf; (void)errbuf_size;
    return 0;
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
            + body + "\n" + main + "\n",
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
                         "NAN_TRAP pc=0x00001234 op=div.s dst=f7 in=[0,0] out=[nan]")

    def test_finite_operands_producing_inf_are_reported(self):
        _result, reports = self.run_exe(self.exe, {"NUM": "3f800000", "DEN": "00000000"})
        self.assertEqual(reports, ["NAN_TRAP pc=0x00001234 op=div.s dst=f7 in=[1,0] out=[inf]"])

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
                         "NAN_TRAP pc=0x0000abcd op=vdiv.s dst=v3 in=[0,0] out=[nan]")

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
        self.assertEqual(reports, [f"NAN_TRAP pc=0x{self.PC:08x} op=vdiv.s dst=v3 in=[0,0] out=[nan]"])

    def test_finite_result_is_silent(self):
        result, reports = self.run_exe(self.exe, {"NUM": "3f800000", "DEN": "40000000"})
        self.assertEqual(result.stdout.strip(), "RC=1 RESULT=0x3f000000")
        self.assertEqual(reports, [])

    def test_propagating_an_existing_nan_is_not_reported(self):
        _result, reports = self.run_exe(self.exe, {"NUM": "7fc00000", "DEN": "3f800000"})
        self.assertEqual(reports, [])


if __name__ == "__main__":
    unittest.main()
