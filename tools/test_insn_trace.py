# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""The real-PSP instruction-trace differential (#312): fixtures, probe, conversion, gate."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import unittest
from typing import Any, ClassVar

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from psp_oracle import insn_trace, parse_golden  # noqa: E402
import tracediff  # noqa: E402

FIXTURE_DIR = ROOT / "fixtures" / "psp_oracle"

# SHA-256 of the step records (no header) of each fixture's PSP_HARDWARE trace, measured on a
# PSP-3000 / 6.6.1 (docs/HARDWARE_ORACLE.md, PSP-TRACE-001). The raw trace stays private; a
# Nakagawa trace of the same words must reproduce these exact records.
PSP_MEASURED_STREAM_SHA256 = {
    "alu-branch-ls": "6d0acdf1a49be864ffe909991c302d534d8f75b7fa588da3a50d6ff2fd85654c",
    "delay-slots": "4b70f42c94fc8460efd0f81295fdbe9b9c680ae67d59a47a7ff587a2fecbfbec",
    "fpu-state": "d6ab944213f8e5fd57bb018f2fc228a5e18677e8b383e369ce191ab3f3f2e2e7",
    "vfpu-state": "a42868073c7695c17dbb3b7775e0f036b45e218173d8025511d71ca37bb392aa",
}

REG = {f"${i}": i for i in range(32)}


def _decode(word: int) -> str:
    """Assembly text for the instruction forms the fixtures use (MIPS32/Allegrex encoding)."""
    op, rs, rt, rd, sa, fn = word >> 26, (word >> 21) & 31, (word >> 16) & 31, \
        (word >> 11) & 31, (word >> 6) & 31, word & 63
    imm = word & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    if op == 0:
        special = {0x21: "addu", 0x23: "subu", 0x26: "xor", 0x27: "nor", 0x2A: "slt",
                   0x2B: "sltu"}
        if word == 0:
            return "sll $0, $0, 0"
        if fn in special:
            return f"{special[fn]} ${rd}, ${rs}, ${rt}"
        if fn in (0x00, 0x03):
            return f"{'sll' if fn == 0 else 'sra'} ${rd}, ${rt}, {sa}"
        if fn == 0x06:
            return f"srlv ${rd}, ${rt}, ${rs}"
        if fn in (0x18, 0x1B):
            return f"{'mult' if fn == 0x18 else 'divu'} ${rs}, ${rt}"
        if fn in (0x10, 0x12):
            return f"{'mfhi' if fn == 0x10 else 'mflo'} ${rd}"
        if fn == 0x08:
            return f"jr ${rs}"
    if op == 1:
        name = {0x01: "bgez", 0x02: "bltzl", 0x10: "bltzal", 0x11: "bgezal"}[rt]
        return f"{name} ${rs}, {simm}"
    if op == 0x11:
        ft, fs, fd, funct = rt, (word >> 11) & 31, (word >> 6) & 31, word & 63
        if rs == 0:
            return "mfc1 $" + str(rt) + ", $f" + str(fs)
        if rs == 2:
            return "cfc1 $" + str(rt) + ", $" + str(fs)
        if rs == 16 and funct == 0:
            return f"add.s $f{fd}, $f{fs}, $f{ft}"
        if rs == 16 and funct == 0x32:
            return f"c.eq.s $f{fs}, $f{ft}"
    if word == 0x48EA0000:
        return "mtv $10, S000"
    if word == 0x486B0000:
        return "mfv $11, S000"
    if word == 0x486E0008:
        return "mfv $14, S200"
    if word == 0x486C0020:
        return "mfv $12, S001"
    if op in (4, 5, 20, 21):
        name = {4: "beq", 5: "bne", 20: "beql", 21: "bnel"}[op]
        return f"{name} ${rs}, ${rt}, {simm}"
    if op in (6, 7):
        return f"{'blez' if op == 6 else 'bgtz'} ${rs}, {simm}"
    if op in (9, 13):
        return f"{'addiu' if op == 9 else 'ori'} ${rt}, ${rs}, " + (str(simm) if op == 9 else f"0x{imm:x}")
    if op == 15:
        return f"lui ${rt}, 0x{imm:x}"
    mem = {0x20: "lb", 0x21: "lh", 0x23: "lw", 0x24: "lbu", 0x28: "sb", 0x29: "sh", 0x2B: "sw"}
    if op in mem:
        return f"{mem[op]} ${rt}, {simm}(${rs})"
    raise AssertionError(f"word 0x{word:08x} is outside the fixtures' forms")


def _gcc() -> str | None:
    return shutil.which(os.environ.get("CC", "gcc"))


class FixtureHeaderTests(unittest.TestCase):
    data: ClassVar[dict[str, Any]]

    @classmethod
    def setUpClass(cls) -> None:
        cls.data = insn_trace.parse_fixtures()

    def test_table_and_constants(self) -> None:
        ids = [f["id"] for f in self.data["fixtures"]]
        self.assertEqual(ids, ["alu-branch-ls", "delay-slots", "fpu-state", "vfpu-state"])
        self.assertEqual(self.data["consts"]["NT_DATA_BASE"], 0x09F00000)
        self.assertEqual(self.data["consts"]["NT_DATA_BYTES"], 0x1000)
        self.assertEqual([f["base"] for f in self.data["fixtures"]],
                         [0x08900000, 0x08910000, 0x08920000, 0x08930000])
        self.assertEqual([len(f["words"]) for f in self.data["fixtures"]], [29, 35, 4, 5])

    def test_selected_fpu_vfpu_initial_state_is_source_owned(self) -> None:
        state = insn_trace.initial_state(self.data["consts"])
        self.assertEqual((state["f0"], state["f1"], state["f2"], state["fcr31"]),
                         (0x3FC00000, 0x40100000, 0, 0))
        self.assertEqual((state["v0"], state["v1"], state["v2"], state["v3"], state["v32"]),
                         (0x3F000000, 0x40000000, 0, 0, 0x40400000))

    def test_every_word_matches_its_assembly_comment(self) -> None:
        for fixture in self.data["fixtures"]:
            for word, asm in zip(fixture["words"], fixture["asm"], strict=True):
                with self.subTest(fixture=fixture["id"], word=f"0x{word:08x}"):
                    self.assertEqual(_decode(word), re.sub(r"\s+\([a-z]+\)$", "", asm))

    def test_no_word_names_the_trampoline_registers(self) -> None:
        """r26..r28 are the trampoline's: no field of any fixture word names them."""
        for fixture in self.data["fixtures"]:
            for word in fixture["words"]:
                fields = {(word >> 21) & 31, (word >> 16) & 31}
                if word >> 26 == 0:
                    fields.add((word >> 11) & 31)
                self.assertFalse(fields & {26, 27, 28}, f"0x{word:08x}")

    def test_delay_slots_hold_no_control_transfer(self) -> None:
        def control(word: int) -> bool:
            op = word >> 26
            return op in (1, 2, 3, 4, 5, 6, 7, 20, 21, 22, 23) or (op == 0 and (word & 63) in (8, 9))
        for fixture in self.data["fixtures"]:
            words = fixture["words"]
            for i, word in enumerate(words[:-1]):
                if control(word):
                    self.assertFalse(control(words[i + 1]), f"{fixture['id']} word {i + 1}")
            self.assertFalse(control(words[-1]), "the last word is not a branch with no delay slot")

    def test_initial_state_contract(self) -> None:
        state = insn_trace.initial_state(self.data["consts"])
        self.assertEqual(state["r4"], 0x09F00000)
        self.assertEqual(state["r29"], 0x09F00800)
        self.assertEqual((state["r26"], state["r27"], state["r28"]), (0, 0, 0))
        self.assertEqual(state["r9"], 0x09090909)
        self.assertEqual((state["hi"], state["lo"]), (0x0BADF00D, 0x13579BDF))
        self.assertEqual((state["f0"], state["f1"], state["f2"], state["fcr31"]),
                         (0x3FC00000, 0x40100000, 0, 0))
        self.assertEqual((state["v0"], state["v1"], state["v2"], state["v3"], state["v32"]),
                         (0x3F000000, 0x40000000, 0, 0, 0x40400000))
        probe = (FIXTURE_DIR / "probe_insn_trace.c").read_text(encoding="utf-8")
        self.assertIn("for (unsigned i = 1; i < 32u; i++) s_nt.r[i] = 0x01010101u * i;", probe)
        self.assertIn("s_nt.r[29] = NT_DATA_BASE + 0x800u;", probe)
        self.assertIn("s_nt.f[0] = NT_FPU_F0_INIT;", probe)
        self.assertIn("s_nt.fcr31 = NT_FCR_INIT;", probe)
        self.assertIn("s_nt.v[0] = NT_VFPU_S000_INIT;", probe)


class ProbeSourceTests(unittest.TestCase):
    probe: ClassVar[str]
    tramp: ClassVar[str]
    issues: ClassVar[str]
    makefile: ClassVar[str]

    @classmethod
    def setUpClass(cls) -> None:
        cls.probe = (FIXTURE_DIR / "probe_insn_trace.c").read_text(encoding="utf-8")
        cls.tramp = (FIXTURE_DIR / "insn_trace_tramp.S").read_text(encoding="utf-8")
        cls.issues = (FIXTURE_DIR / "probe_hle_issues.c").read_text(encoding="utf-8")
        cls.makefile = (FIXTURE_DIR / "Makefile").read_text(encoding="utf-8")

    def test_route_registration_and_log(self) -> None:
        from psp_oracle.run_psplink import (  # noqa: PLC0415
            CAMPAIGN_QUEUE_CASES, _campaign_completeness_contract, _campaign_host0_log_path)
        self.assertIn("else ifeq ($(CASE),insn-trace)\nCASE_ID = 119\n", self.makefile)
        self.assertIn("else ifeq ($(CASE),insn-trace)\nTARGET = $(BUILD_DIR)/insn_trace\n"
                      "OBJS = $(BUILD_DIR)/probe_insn_trace.o $(BUILD_DIR)/insn_trace_tramp.o "
                      "$(BUILD_DIR)/threadman_user_imports.o\n", self.makefile)
        self.assertIn('#define HLE_CAMPAIGN_ID "insn-trace"\n#define HLE_TEST_ID "PSP-TRACE-001"\n'
                      '#define HLE_LOG "host0:/insn_trace_log.txt"', self.probe)
        self.assertEqual(_campaign_host0_log_path(Path("/scratch"), "insn-trace").name,
                         "insn_trace_log.txt")
        self.assertEqual(_campaign_completeness_contract("insn-trace"), "strict-golden-sequence")
        self.assertNotIn("insn-trace", CAMPAIGN_QUEUE_CASES)

    def test_exception_fixture_is_a_registered_one_launch_probe(self) -> None:
        route = "else ifeq ($(CASE),insn-trace-exception)\nCASE_ID = 135\n"
        self.assertIn(route, self.makefile)
        spec, counts = parse_golden.CAMPAIGN_PROBE_CASES["insn-trace-exception"]
        self.assertEqual(spec.test_id, "PSP-TRACE-EXCEPTION-001")
        self.assertEqual(spec.ordered_cases, ("exception-armed",))
        self.assertEqual(counts["exception-armed"], 1)
        self.assertIn("NT_CASE_EXCEPTION 135", self.probe)
        self.assertIn("beq $0, $0, 1f", self.probe)
        self.assertIn('"nt_exception_break:\\n\\t"', self.probe)
        self.assertIn('"break\\n\\t"', self.probe)

    def test_writer_round_trip_and_lifecycle_are_the_hle_issues_copies(self) -> None:
        shared = self.issues[self.issues.index("static int s_log_failed;\n"):
                             self.issues.index("/* ---", self.issues.index("int module_stop("))]
        self.assertIn(shared, self.probe)

    def test_trampoline_layout_matches_the_probe_state(self) -> None:
        self.assertIn("uint32_t r[32];\n    uint32_t hi, lo;\n    uint32_t flag;\n"
                      "    uint32_t f[4];\n    uint32_t fcr31;\n    uint32_t v[5];\n"
                      "    uint32_t host[12];\n    uint32_t host_f[4];\n"
                      "    uint32_t host_fcr31;\n    uint32_t host_v[5];", self.probe)
        loads = re.findall(r"lw\s+\$(\d+), (\d+)\(\$28\)", self.tramp)
        stores = re.findall(r"sw\s+\$(\d+), (\d+)\(\$28\)", self.tramp)
        traced = [str(r) for r in insn_trace.TRACED_GPRS]
        self.assertEqual([r for r, _ in loads if r != "1"][:len(traced) - 1], traced[1:])
        for reg, off in loads + stores:
            if off not in ("128", "132", "136", "140", "144", "148", "152", "156"):
                self.assertEqual(int(off), 4 * int(reg))
        self.assertNotIn("$26", self.tramp)
        self.assertNotIn("$27", self.tramp)
        self.assertEqual(self.tramp.count("nt_tramp_slot:\n    nop\n    nop\n"), 1)
        self.assertIn("nt_tramp_taken:\n    sw    $28, 136($28)", self.tramp)
        self.assertIn("lw    $1, 140($28)\n    mtc1  $1, $f0", self.tramp)
        self.assertIn("swc1  $f0, 140($28)", self.tramp)
        self.assertIn("lv.s  S000, 160($28)", self.tramp)
        self.assertIn("sv.s  S000, 160($28)", self.tramp)
        self.assertIn("lv.s  S200, 176($28)", self.tramp)
        self.assertIn("sv.s  S200, 176($28)", self.tramp)

    def test_steps_are_bounded_and_unsupported_words_stop(self) -> None:
        self.assertIn("#define NT_MAX_STEPS 512u", self.probe)
        self.assertIn("#define NT_TRAMP_WORDS 192u", self.probe)
        self.assertIn("while (step < NT_MAX_STEPS) {", self.probe)
        self.assertIn("status = NT_END_UNSUPPORTED;", self.probe)
        self.assertIn("sceKernelIcacheInvalidateRange(s_nt_tramp, s_nt_tramp_words * 4u);", self.probe)

    def test_registry_shape(self) -> None:
        spec, counts = parse_golden.CAMPAIGN_PROBE_CASES["insn-trace"]
        self.assertEqual(spec.test_id, "PSP-TRACE-001")
        self.assertEqual(spec.terminal_case, "insn-trace-done")
        self.assertEqual(len(parse_golden.INSN_TRACE_CASES),
                         (105 + 2) + (27 + 2) + (4 + 2) + (5 + 2))
        self.assertEqual(counts["alu-branch-ls-init"], 44)
        self.assertEqual(counts["delay-slots-s0026"], insn_trace.RECORD_WORDS)
        self.assertEqual(counts["fpu-state-s0000"], insn_trace.RECORD_WORDS)
        self.assertEqual(counts["vfpu-state-s0004"], insn_trace.RECORD_WORDS)
        self.assertEqual(counts["delay-slots-end"], 4)


class ConversionTests(unittest.TestCase):
    """The PSP log converter: identity checks and changed-register diffs."""

    def _log(self, fixture, steps, tamper=None):
        data = insn_trace.parse_fixtures()
        state = insn_trace.initial_state(data["consts"])
        names = list(insn_trace.TRACE_STATE_NAMES)
        commit = "1" * 40
        lines = [f"NAKAGAWA_PSP_META schema=1 source=psp source_commit={commit} fixture=hle-measure"]

        def rec(case, outs, status="PASS", result=0):
            text = "".join(f" out{i}=0x{v:08x}" for i, v in enumerate(outs))
            lines.append(f"NAKAGAWA_PSP_TEST schema=1 test_id=PSP-TRACE-001 case_id={fixture['id']}-{case} "
                         f"status={status} result=0x{result:08x}{text}")
        fnv = insn_trace.words_fnv(fixture["words"])
        rec("init", [fixture["base"], len(fixture["words"]), fnv, data["consts"]["NT_DATA_BASE"]]
            + [state[n] for n in names])
        for i, (pc, op, changes, store) in enumerate(steps):
            state.update(changes)
            rec(f"s{i:04d}", [pc, op] + [state[n] for n in names] + list(store[1:]))
        rec("end", [len(steps), 0, 0, 0])
        text = "\n".join(lines) + "\n"
        return (tamper(text) if tamper else text), data

    def test_changed_registers_and_stores_become_v2_tokens(self) -> None:
        fixture = insn_trace.parse_fixtures()["fixtures"][0]
        steps = [(0x08900000, 0x24080005, {"r8": 5}, (0, 0, 0)),
                 (0x08900004, 0xAC8A0000, {}, (32, 0x09F00000, 0x12345678)),
                 (0x08900008, 0xA08B0004, {"hi": 1, "lo": 2}, (8, 0x09F00004, 0x78)),
                 (0x0890000C, 0x46010080, {"f2": 0x40700000}, (0, 0, 0)),
                 (0x08900010, 0x46021032, {"fcr31": 0x00800000}, (0, 0, 0)),
                 (0x08900014, 0x48EA0000, {"v0": 0x0A0A0A0A}, (0, 0, 0))]
        text, data = self._log(fixture, steps)
        lines, commit = insn_trace.psp_records(text, fixture, data["consts"])
        self.assertEqual(commit, "1" * 40)
        self.assertEqual(lines, ["0 pc=0x08900000 op=0x24080005 r8=0x00000005",
                                 "1 pc=0x08900004 op=0xac8a0000 m32[0x09f00000]=0x12345678",
                                 "2 pc=0x08900008 op=0xa08b0004 hi=0x00000001 lo=0x00000002 "
                                 "m8[0x09f00004]=0x78",
                                 "3 pc=0x0890000c op=0x46010080 f2=0x40700000",
                                 "4 pc=0x08900010 op=0x46021032 fcr31=0x00800000",
                                 "5 pc=0x08900014 op=0x48ea0000 v0=0x0a0a0a0a"])

    def test_stale_or_foreign_runs_are_refused(self) -> None:
        fixture = insn_trace.parse_fixtures()["fixtures"][0]
        steps = [(0x08900000, 0x24080005, {"r8": 5}, (0, 0, 0))]
        for tamper, why in (
                (lambda t: t.replace(f"out2=0x{insn_trace.words_fnv(fixture['words']):08x}",
                                     "out2=0x00000000", 1), "FNV"),
                (lambda t: t.replace("-end status=PASS result=0x00000000",
                                     "-end status=FAIL result=0x00000002"), "end address"),
                (lambda t: t.replace("source_commit=", "source_kommit="), "META"),
                (lambda t: "\n".join(x for x in t.splitlines() if "-s0000 " not in x), "s0000")):
            text, data = self._log(fixture, steps, tamper)
            with self.subTest(why=why), self.assertRaisesRegex(insn_trace.InsnTraceError, why):
                insn_trace.psp_records(text, fixture, data["consts"])

    def test_v2_header_identity(self) -> None:
        fixture = insn_trace.parse_fixtures()["fixtures"][1]
        header = insn_trace.v2_header("PSP_HARDWARE", fixture, 27, source_commit="a" * 40,
                                      model="PSP-3000", firmware="6.6.1")
        self.assertIn(f"binary_sha256={insn_trace.image_sha256(fixture)}", header)
        self.assertIn("fixture_id=insn-trace cell_id=delay-slots", header)
        self.assertIn("start_pc=0x08910000 steps=27 complete=1", header)

    def test_exception_frame_tokens_are_valid_strict_trace_fields(self) -> None:
        fixture = insn_trace.parse_fixtures()["fixtures"][0]
        header_psp = insn_trace.v2_header(
            "PSP_HARDWARE", fixture, 1, source_commit="a" * 40,
            model="PSP-3000", firmware="6.6.0",
        )
        header_local = header_psp.replace("source_tier=PSP_HARDWARE", "source_tier=LOCAL_COSIM")
        frame = ("0 pc=0x08900000 op=0x0000000d epc=0x08900000 cause=0x80000024 "
                 "badvaddr=0x00000000 status=0x00000010")
        with tempfile.TemporaryDirectory(prefix="insn-trace-exception-") as tmp_name:
            psp = Path(tmp_name) / "exception.psp.trace"
            local = Path(tmp_name) / "exception.local.trace"
            insn_trace.write_trace(psp, header_psp, [frame])
            insn_trace.write_trace(local, header_local, [frame])
            self.assertIsNone(tracediff.strict_hardware_diff(psp, local))


@unittest.skipUnless(_gcc(), "needs a host C compiler for the generated code")
class LocalTraceTests(unittest.TestCase):
    """Nakagawa's trace of the fixtures, the measured PSP streams, and a codegen mutation."""

    data: ClassVar[dict[str, Any]]
    tmp: ClassVar[tempfile.TemporaryDirectory[str]]
    lines: ClassVar[dict[str, list[str]]]

    @classmethod
    def setUpClass(cls) -> None:
        cls.data = insn_trace.parse_fixtures()
        cls.tmp = tempfile.TemporaryDirectory()
        cls.lines = {}
        for fixture in cls.data["fixtures"]:
            cls.lines[fixture["id"]] = insn_trace.local_records(
                fixture, cls.data["consts"], Path(cls.tmp.name) / fixture["id"])

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_step_counts_match_the_probe_registry(self) -> None:
        self.assertEqual({fid: len(lines) for fid, lines in self.lines.items()},
                         dict(parse_golden.INSN_TRACE_STEPS))

    def test_delay_slot_records(self) -> None:
        ds = self.lines["delay-slots"]
        self.assertEqual(ds[3], "3 pc=0x0891000c op=0x51000016")
        self.assertEqual(ds[4].split()[1], "pc=0x08910014", "the annulled beql delay slot has no record")
        self.assertEqual(ds[6], "6 pc=0x08910020 op=0x04110014 r31=0x08910028")
        self.assertEqual(ds[16], "16 pc=0x08910044 op=0xac880000 m32[0x09f00000]=0xfffffffb")

    def test_fpu_vfpu_register_state_is_seeded_and_traced(self) -> None:
        fpu = self.lines["fpu-state"]
        vfpu = self.lines["vfpu-state"]
        self.assertIn("f2=0x40700000", fpu[0])
        self.assertIn("r8=0x40700000", fpu[1])
        self.assertIn("fcr31=0x00800000", fpu[2])
        self.assertIn("r9=0x00800000", fpu[3])
        self.assertIn("r11=0x3f000000", vfpu[0])
        self.assertIn("r12=0x40000000", vfpu[1])
        self.assertIn("r14=0x40400000", vfpu[2])
        self.assertIn("v0=0x0a0a0a0a", vfpu[3])
        self.assertIn("r11=0x0a0a0a0a", vfpu[4])

    def test_fpu_vfpu_mutations_diverge_at_the_first_changed_state(self) -> None:
        def compare_mutant(fixture_id: str, mutate, expected_step: int, token: str) -> None:
            fixture = next(item for item in self.data["fixtures"] if item["id"] == fixture_id)
            mutant = insn_trace.local_records(
                fixture, self.data["consts"], Path(self.tmp.name) / f"{fixture_id}-mutant", mutate
            )
            good_path = Path(self.tmp.name) / f"{fixture_id}-good.trace"
            bad_path = Path(self.tmp.name) / f"{fixture_id}-mutant.trace"
            header = f"# psp-recomp trace v1 oracle=recomp start_pc=0x{fixture['base']:08x}"
            insn_trace.write_trace(good_path, header, self.lines[fixture_id])
            insn_trace.write_trace(bad_path, header, mutant)
            divergence = tracediff.strict_local_diff(str(good_path), str(bad_path))
            self.assertIsNotNone(divergence)
            step, _pc, detail = divergence
            self.assertEqual(step, expected_step)
            self.assertIn(token, detail)

        def mutate_fpu(gen_c: str) -> None:
            changed = 0
            for path in Path(gen_c).parent.glob("gen*.c"):
                text = path.read_text(encoding="utf-8")
                if "sr_fpu_add_s" in text:
                    path.write_text(text.replace("sr_fpu_add_s", "sr_fpu_sub_s"),
                                    encoding="utf-8", newline="\n")
                    changed += 1
            self.assertGreater(changed, 0)

        def mutate_vfpu(gen_c: str) -> None:
            changed = 0
            for path in Path(gen_c).parent.glob("gen*.c"):
                text = path.read_text(encoding="utf-8")
                if "s->vi[0] = s->r[10];" in text:
                    path.write_text(text.replace("s->vi[0] = s->r[10];", "s->vi[0] = 0;"),
                                    encoding="utf-8", newline="\n")
                    changed += 1
            self.assertGreater(changed, 0)

        compare_mutant("fpu-state", mutate_fpu, 0, "f2")
        compare_mutant("vfpu-state", mutate_vfpu, 3, "v0")

    def test_streams_reproduce_the_psp_measurement(self) -> None:
        # New source-owned fixtures stay unmeasured until a matching PSP capture is cited.
        for fid in PSP_MEASURED_STREAM_SHA256:
            lines = self.lines[fid]
            digest = hashlib.sha256("".join(line + "\n" for line in lines).encode()).hexdigest()
            self.assertEqual(digest, PSP_MEASURED_STREAM_SHA256[fid], fid)

    def test_a_codegen_mutation_is_caught_at_its_first_instruction(self) -> None:
        """xor emitted as or: the comparator names step 13, the first xor, and its write."""
        def mutate(gen_c: str) -> None:
            for path in Path(gen_c).parent.glob("gen*.c"):
                text = path.read_text(encoding="utf-8")
                line = "s->r[16] = (s->r[14] ^ s->r[15]);"
                if line in text:
                    path.write_text(text.replace(line, "s->r[16] = (s->r[14] | s->r[15]);"),
                                    encoding="utf-8", newline="\n")
        fixture = self.data["fixtures"][0]
        work = Path(self.tmp.name) / "mutant"
        mutated = insn_trace.local_records(fixture, self.data["consts"], work, mutate=mutate)
        good_path = Path(self.tmp.name) / "good.trace"
        bad_path = Path(self.tmp.name) / "bad.trace"
        header = "# psp-recomp trace v1 oracle=recomp start_pc=0x08900000"
        insn_trace.write_trace(good_path, header, self.lines[fixture["id"]])
        insn_trace.write_trace(bad_path, header, mutated)
        divergence = tracediff.strict_local_diff(str(good_path), str(bad_path))
        self.assertIsNotNone(divergence)
        step, pc, detail = divergence
        self.assertEqual((step, pc), (13, "0x08900034"))
        self.assertIn("r16", detail)


if __name__ == "__main__":
    unittest.main()
