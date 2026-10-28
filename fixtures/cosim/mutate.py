# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Prove the AOT/interpreter and FPU-reference gates are load-bearing.

A comparator that cannot fail proves nothing. This driver applies one semantic
defect at a time to a copy of the production interpreter, FPU helper header,
code generator or scheduler under the ignored build tree. Interpreter/generator
mutants rebuild the cosim harness; the scheduler mutant builds a copied HLE
thread harness with the real scheduler, recomp.c and blocking HLE wait.

Two rules make the campaign meaningful rather than decorative:

* A mutant must BUILD. A compile error is not a kill -- it proves the compiler
  noticed, not the comparator -- so a mutant whose harness never ran is reported
  as INVALID and fails the campaign.
* A mutant must change behavior the comparator claims to cover. Each entry below
  names the mission-level defect class it stands for, and the driver records the
  first divergence the gate actually reported, so a mutant that fails "for some
  other reason" is visible in the output rather than counted silently.
* A verdict is bound to the process outcome. A marker is never classified on its
  own: "cosim: OK" from a nonzero process is an inconsistent run, a bare
  "cosim: FAIL" with no located divergence is not a kill, and a gate that exceeds
  its budget is a TIMEOUT -- never a pass and never a kill.

The source tree is never modified. Mutants are selected through Makefile
overrides; the scheduler mutant copies sched.c beside the copied HLE-thread
harness so its quoted include resolves only to the mutated file.

Each gate is a serial build, so the campaign runs its mutants in parallel lanes
(--jobs, one per CPU by default). Every lane builds beneath its own BUILD_ROOT
under <build-dir>/lanes/ and must pass the unmutated gate there before it judges
a mutant; scheduler mutants form one lane of their own. The developer's own
build trees are neither mutated nor rebuilt.
"""

from __future__ import annotations

import argparse
from concurrent.futures import Future, ThreadPoolExecutor
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import threading


ROOT = Path(__file__).resolve().parents[2]
INTERPRETER = ROOT / "src" / "rt" / "guest_interp.c"
FP_CONVERT = ROOT / "src" / "rt" / "fp_convert.h"
FPU_REFERENCE = ROOT / "fixtures" / "cosim" / "fpu_reference.c"
FPU_REFERENCE_HEADER = ROOT / "fixtures" / "cosim" / "fpu_reference.h"
GENERATOR = ROOT / "tools" / "codegen.py"
SCHEDULER = ROOT / "src" / "rt" / "sched.c"
HLE_THREAD_SELFTEST = ROOT / "src" / "rt" / "hle_thread_selftest.c"


class Mutant:
    """One semantic defect, expressed as exact text substitutions.

    Every substitution declares how many occurrences it must replace.  A
    substitution that stops matching -- because the interpreter was refactored --
    is a hard error, not a silently skipped mutant: that is exactly how a
    mutation campaign rots into a no-op.
    """

    def __init__(
        self,
        name: str,
        defect_class: str,
        substitutions: list[tuple[str, str, int]],
        target: str = "interpreter",
    ):
        self.name = name
        self.defect_class = defect_class
        self.substitutions = substitutions
        # Which side of the differential this mutant damages. A comparator proven
        # against one side only is half proven: an AOT-side defect is exactly the
        # class this gate was built to find, so at least one mutant must be one.
        self.target = target

    def apply(self, source: str) -> str:
        mutated = source
        for old, new, expected in self.substitutions:
            found = mutated.count(old)
            if found != expected:
                raise RuntimeError(
                    f"mutant {self.name!r}: pattern matched {found} time(s), expected "
                    f"{expected}. The interpreter changed shape; update the mutant "
                    f"instead of letting the campaign lose a defect class.\n"
                    f"pattern: {old!r}"
                )
            mutated = mutated.replace(old, new)
        if mutated == source:
            raise RuntimeError(f"mutant {self.name!r} did not change the source")
        return mutated


MUTANTS: tuple[Mutant, ...] = (
    Mutant(
        "skip-delay-slot",
        "skip delay slot",
        [(
            "            SrGuestInterpResult delay_result = execute_noncontrol(\n"
            "                s, pc + 4u, delay_opcode, &delay_store_address, "
            "&delay_store_size, fault);\n",
            "            SrGuestInterpResult delay_result = SR_GUEST_INTERP_AOT_HANDOFF;\n",
            1,
        )],
    ),
    Mutant(
        "allow-r0-write",
        "allow r0 write",
        [
            (
                "static void write_gpr(CpuState *s, uint32_t index, uint32_t value) {\n"
                "    if (index != 0u) {\n"
                "        s->r[index] = value;\n"
                "    }\n"
                "}",
                "static void write_gpr(CpuState *s, uint32_t index, uint32_t value) {\n"
                "    s->r[index] = value;\n"
                "}",
                1,
            ),
            # $r0 suppression is enforced twice: the guarded write above and the
            # per-instruction restore below. Removing only one leaves the other
            # repairing the damage, so the mutant would survive for a reason that
            # has nothing to do with the comparator. Remove the whole mechanism. The
            # count is the number of per-instruction restore sites in guest_interp.c
            # (the unaligned word forms added the eleventh and the SPECIAL3 forms
            # the twelfth); a new site bumps it.
            ("s->r[0] = 0u;", "(void)0;", 12),
        ],
    ),
    Mutant(
        "linked-call-hands-off",
        "a linked transfer into a registered body hands off instead of calling it",
        [(
            "            if (transfer.taken && transfer.link_register >= 0 &&\n",
            "            if (0 && transfer.taken && transfer.link_register >= 0 &&\n",
            1,
        )],
    ),
    Mutant(
        "wrong-call-resume",
        "an interpreted call resumes at the delay slot instead of the link address",
        [(
            "                const uint32_t resume_pc = pc + 8u;\n",
            "                const uint32_t resume_pc = pc + 4u;\n",
            1,
        )],
    ),
    Mutant(
        "wrong-branch-target",
        "incorrect branch target",
        [(
            # Four decoders share this expression: beq/bne and their likely
            # forms, blezl/bgtzl, bltzl/bgezl, and the bc1 group.
            "        out->target = pc + 4u + (sign_extend_16(opcode) << 2);",
            "        out->target = pc + 8u + (sign_extend_16(opcode) << 2);",
            4,
        )],
    ),
    Mutant(
        "wrong-jump-target",
        "incorrect branch target (direct jump form)",
        [(
            "        out->target = ((pc + 4u) & 0xf0000000u) | ((opcode & 0x03ffffffu) << 2);",
            "        out->target = ((pc + 4u) & 0xf0000000u) | "
            "(((opcode & 0x03ffffffu) << 2) + 4u);",
            1,
        )],
    ),
    Mutant(
        "likely-slot-not-annulled",
        "a not-taken branch-likely still executes its delay slot",
        [(
            "            if (transfer.likely && !transfer.taken) {\n",
            "            if (0 && transfer.likely && !transfer.taken) {\n",
            1,
        )],
    ),
    Mutant(
        "bc1-inverted-sense",
        "bc1t/bc1f test the wrong sense of FCC0",
        [(
            "        out->taken = tf != 0u ? condition : !condition;",
            "        out->taken = tf != 0u ? !condition : condition;",
            1,
        )],
    ),
    Mutant(
        "movn-inverted",
        "movn writes when its condition register is zero",
        [(
            "if (read_gpr(s, rt) != 0u) write_gpr(s, rd, read_gpr(s, rs));",
            "if (read_gpr(s, rt) == 0u) write_gpr(s, rd, read_gpr(s, rs));",
            1,
        )],
    ),
    Mutant(
        "ext-size-off-by-one",
        "ext extracts one bit fewer than its size field encodes",
        [(
            "            const uint32_t size = rd + 1u;\n",
            "            const uint32_t size = rd;\n",
            1,
        )],
    ),
    Mutant(
        "ins-keeps-old-field",
        "ins ORs into the destination instead of replacing the field",
        [(
            "write_gpr(s, rt, (old_value & ~field_mask) | inserted);",
            "write_gpr(s, rt, old_value | inserted);",
            1,
        )],
    ),
    Mutant(
        "seb-zero-extends",
        "seb fails to sign-extend a negative byte",
        [(
            "write_gpr(s, rd, (byte & 0x80u) != 0u ? byte | 0xffffff00u : byte);",
            "write_gpr(s, rd, byte);",
            1,
        )],
    ),
    Mutant(
        "mov-s-quiets-nan",
        "mov.s quiets a signaling NaN instead of copying the raw word",
        [(
            "            s->fi[fd] = s->fi[fs];\n",
            "            s->fi[fd] = ((s->fi[fs] & 0x7f800000u) == 0x7f800000u &&\n"
            "                         (s->fi[fs] & 0x007fffffu) != 0u)\n"
            "                ? (s->fi[fs] | 0x00400000u) : s->fi[fs];\n",
            1,
        )],
    ),
    Mutant(
        "omit-halfword-store",
        "omit one memory write",
        [(
            "                case 0x29u: MEM_W16_PC(address, read_gpr(s, rt), pc); break;",
            "                case 0x29u: break;",
            1,
        )],
    ),
    Mutant(
        "alter-handback-pc",
        "alter handback PC",
        [(
            "        if (sr_lookup(pc) && !sr_stale_block_is_stale(pc)) {\n            s->pc = pc;",
            "        if (sr_lookup(pc) && !sr_stale_block_is_stale(pc)) {\n            s->pc = pc + 4u;",
            1,
        )],
    ),
    Mutant(
        "alter-link-value",
        "alter handback PC (link-register form)",
        [(
            "                write_gpr(s, (uint32_t)transfer.link_register, pc + 8u);",
            "                write_gpr(s, (uint32_t)transfer.link_register, pc + 4u);",
            1,
        )],
    ),
    Mutant(
        "aot-ra-return-boundary-ignored",
        "AOT accepts a linked callee that rewrites $ra",
        [
            (
                '        f"if (sr_aot_return_boundary_begin(s, {G(resume_pc)}, {G(call_pc)}, 1u)) return; "',
                '        ""',
                1,
            ),
            (
                '        "if (sr_aot_return_boundary_end(s)) return;"',
                '        ""',
                1,
            ),
        ],
        "generator",
    ),
    Mutant(
        "drop-aot-thread-boundary-swap",
        "scheduler fails to restore a guest thread's AOT return-boundary stack",
        [(
            "        sr_aot_return_boundary_stack_swap(&t->aot_return_boundary_stack);\n",
            "        /* mutant: omitted per-thread return-boundary swap */\n",
            2,
        )],
        "scheduler",
    ),
    Mutant(
        "drop-mfhi",
        "fail to materialize one architectural register",
        [(
            "        case 0x10u: write_gpr(s, rd, s->hi); break;",
            "        case 0x10u: break;",
            1,
        )],
    ),
    Mutant(
        "wrong-byte-extension",
        "fail to materialize one architectural register (sign extension)",
        [(
            "                case 0x20u: /* lb -- sign extended */\n"
            "                    write_gpr(s, rt, (uint32_t)(int32_t)(int8_t)MEM_R8(address));",
            "                case 0x20u: /* lb -- sign extended */\n"
            "                    write_gpr(s, rt, MEM_R8(address));",
            1,
        )],
    ),
    Mutant(
        "sc-ignores-link",
        "store-conditional succeeds without a link",
        [(
            "                    const uint32_t linked = s->llbit != 0u ? 1u : 0u;",
            "                    const uint32_t linked = 1u;",
            1,
        )],
    ),
    Mutant(
        "codegen-sc-ignores-link",
        "GENERATOR: store-conditional succeeds without a link",
        [(
            "uint32_t _sc = s->llbit != 0u ? 1u : 0u; ",
            "uint32_t _sc = 1u; ",
            1,
        )],
        target="generator",
    ),
    Mutant(
        "codegen-likely-slot-always-runs",
        "GENERATOR: a not-taken branch-likely still runs its delay slot",
        [(
            "            if is_likely(w):\n",
            "            if False and is_likely(w):\n",
            2,
        )],
        target="generator",
    ),
    Mutant(
        "codegen-ins-keeps-old-field",
        "GENERATOR: ins ORs into the destination instead of replacing the field",
        [(
            'f"(({R(rt(w))} & ~0x{mask:x}u) | (({R(rs(w))} << {pos}) & 0x{mask:x}u))"',
            'f"({R(rt(w))} | (({R(rs(w))} << {pos}) & 0x{mask:x}u))"',
            1,
        )],
        target="generator",
    ),
    Mutant(
        "codegen-computed-transfer-timing",
        "GENERATOR: a computed transfer reads its target after its delay slot",
        [
            # Exactly the production defect this fixture was built to find
            # (fixed by #126). The generator is restored to the pre-fix emission
            # order, in which the `jalr` target register is read AFTER the delay
            # slot has already run -- so a slot that writes that register
            # redirects the call. Cell `jrslot` must diverge. Guest addresses are
            # spelled through the generator's address space, G(addr), which a
            # runtime-placed module translates base-relative.
            (
                "            out.append(f\"    {{ uint32_t _t = {R(a)};\")\n"
                "            out.append(f\"      sr_begin(s, {G(addr)}, 0x{w:08x}u); "
                "{link}sr_end(s, 0u, 0);\")\n"
                "            out.extend(delay_slot_lines(ds, dsw, addr, hst_profile=hst_profile, lle_cpu=lle_cpu, resumable=resumable,\n"
                "                                        indent=\"      \"))",
                "            out.append(f\"    sr_begin(s, {G(addr)}, 0x{w:08x}u); "
                "{link}sr_end(s, 0u, 0);\")\n"
                "            out.extend(delay_slot_lines(ds, dsw, addr, hst_profile=hst_profile, lle_cpu=lle_cpu, resumable=resumable,\n"
                "                                        indent=\"      \"))\n"
                "            out.append(f\"    {{ uint32_t _t = {R(a)};\")",
                1,
            ),
        ],
        target="generator",
    ),
    Mutant(
        "codegen-ctc1-keeps-unimplemented-bits",
        "GENERATOR: ctc1 stores FCR31 bits 25..31, which the PSP FPU does not have",
        [(
            # The pre-measurement emission: ctc1 stored the guest word wholesale.
            # PSP-FPU-CC-001 measured that a ctc1 of 0xFE800000 reads back
            # 0x00800000; the FPU oracle's ctc1/FCR31 check must report it.
            'if ({fs} == 31) {{ s->fcr31 = {R(rt(w))} & ~0xfe000000u; "',
            'if ({fs} == 31) {{ s->fcr31 = {R(rt(w))}; "',
            1,
        )],
        target="generator",
    ),
    Mutant(
        "drop-fcr31-threading",
        "guest FCR31 not threaded into the scalar FPU helper path",
        [(
            "            s->fi[fd] = sr_fpu_to_word(s->f[fs], funct, s->fcr31);",
            "            s->fi[fd] = sr_fpu_to_word(s->f[fs], funct, 0u);",
            1,
        )],
    ),
    Mutant(
        "fpu-wrong-rounding",
        "FPU: break nearest-even ties upward",
        [(
            "    return fmod(lower, 2.0) == 0.0 ? lower : lower + 1.0;",
            "    return lower + 1.0;",
            1,
        )],
        target="fp-convert",
    ),
    Mutant(
        "fpu-wrong-conversion-saturation",
        "FPU: return zero instead of saturating positive overflow",
        [(
            "    if (x >= 2147483648.0) return 0x7fffffffu;",
            "    if (x >= 2147483648.0) return 0u;",
            1,
        )],
        target="fp-convert",
    ),
    Mutant(
        "fpu-wrong-infzero-canonicalization",
        "FPU: return a noncanonical infinity-times-zero NaN",
        [(
            "        const uint32_t canonical_qnan = 0x7fc00000u;",
            "        const uint32_t canonical_qnan = 0xffc00000u;",
            1,
        )],
        target="fp-convert",
    ),
    Mutant(
        "fpu-wrong-nan-compare",
        "FPU: reject unordered C.cond predicates",
        [(
            "    const unsigned unordered = ((a_mag & 0x7f800000u) == 0x7f800000u &&\n"
            "                                (a_mag & 0x007fffffu) != 0u) ||\n"
            "                               ((b_mag & 0x7f800000u) == 0x7f800000u &&\n"
            "                                (b_mag & 0x007fffffu) != 0u);",
            "    const unsigned unordered = 0u;",
            1,
        )],
        target="fp-convert",
    ),
    Mutant(
        "fpu-swap-ccond-8-9",
        "FPU: swap C.cond signaling predicates 8/9",
        [(
            "    return (unordered && (condition & 1u)) ||\n"
            "           (equal && (condition & 2u)) ||\n"
            "           (less && (condition & 4u));",
            "    return condition == 8u ? (unordered != 0u) :\n"
            "           condition == 9u ? 0u :\n"
            "           ((unordered && (condition & 1u)) ||\n"
            "            (equal && (condition & 2u)) ||\n"
            "            (less && (condition & 4u)));",
            1,
        )],
        target="fp-convert",
    ),
    Mutant(
        "fpu-reference-swap-ccond-8-9",
        "FPU reference: swap C.cond signaling predicates 8/9",
        [(
            "    return ref_ccond_truth[condition][relation];",
            "    return ref_ccond_truth[condition == 8u ? 9u :\n"
            "                            condition == 9u ? 8u : condition][relation];",
            1,
        )],
        target="fpu-reference",
    ),
)


def resolve_make() -> str:
    """Find the same make that invoked us.

    GNU Make exports MAKE to every recipe, so a campaign started from
    `make cosim-mutants` re-enters the identical program -- including the flags
    the outer invocation was given, which MAKEFLAGS carries through the
    environment. Falling back to a PATH lookup keeps the script runnable on its
    own; guessing from sys.platform does not survive an MSYS2 shell, where the
    interpreter's platform string does not predict which make exists.
    """
    from_env = os.environ.get("MAKE")
    if from_env:
        return from_env
    for candidate in ("mingw32-make", "make", "gmake"):
        found = shutil.which(candidate)
        if found:
            return found
    raise RuntimeError("no make program found (set MAKE or put make on PATH)")


def lane_overrides(build_root: Path) -> list[str]:
    """Make overrides that put every output of one gate beneath `build_root`.

    BUILD_ROOT alone is not enough. Each make invocation records its build
    profile in its own BUILD_DIR, and recording one profile deletes the stamps
    of every other, so two gates sharing that directory with different overrides
    restart each other's make indefinitely. BUILD_DIR and COSIM_DIR are named
    too, because a caller may have overridden them (tools/compiler_compare.py
    passes COSIM_DIR) and MAKEFLAGS hands that override to every gate; a gate's
    own arguments win over the inherited ones.
    """
    return [
        f"BUILD_ROOT={build_root.as_posix()}",
        f"BUILD_DIR={(build_root / 'gate').as_posix()}",
        f"COSIM_DIR={(build_root / 'cosim').as_posix()}",
    ]


def make_command(source: Path, target: str,
                 harness_source: Path | None = None,
                 build_root: Path | None = None) -> list[str]:
    if target == "interpreter":
        command = ["cosim-selftest", f"COSIM_INTERP_SRC={source.as_posix()}"]
    elif target == "fp-convert":
        command = ["cosim-selftest", f"COSIM_FP_CONVERT_PRELUDE={source.as_posix()}"]
    elif target == "fpu-reference":
        command = ["cosim-selftest", f"COSIM_FPU_REFERENCE_SRC={source.as_posix()}"]
    elif target == "generator":
        command = ["cosim-selftest", f"CODEGEN_TOOL={source.as_posix()}"]
    elif target == "scheduler":
        command = ["aot-return-boundary-thread-selftest"]
        if harness_source is not None:
            command.append(f"HLE_THREAD_SELFTEST_SOURCE={harness_source.as_posix()}")
    else:
        raise ValueError(f"unknown mutation target: {target}")
    if build_root is not None:
        command.extend(lane_overrides(build_root))
    return [resolve_make(), "--no-print-directory", *command]


# A generated-code control-flow regression can hang the driver, so the gate gets a
# separate runtime budget. A timeout is neither PASS nor a semantic kill.
GATE_TIMEOUT_S = int(os.environ.get("COSIM_GATE_TIMEOUT_S", "3600"))
TERMINATE_GRACE_S = 30


def _terminate_process_tree(process: subprocess.Popen) -> None:
    """Reap the gate and the build children it started.

    Host-scoped by design: Windows terminates the tree through taskkill, POSIX
    terminates the session's process group that Popen created below. A surviving
    build child would keep a CI slot busy long after the driver gave up.
    """
    pid = getattr(process, "pid", None)
    if pid is not None:
        if os.name == "nt":
            try:
                subprocess.run(
                    ["taskkill", "/PID", str(pid), "/T", "/F"],
                    capture_output=True, text=True, check=False, timeout=10,
                )
            except (OSError, subprocess.TimeoutExpired):
                pass
        else:
            try:
                os.killpg(os.getpgid(pid), signal.SIGKILL)
            except (OSError, AttributeError):
                pass
    try:
        process.wait(timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        process.kill()
        try:
            process.wait(timeout=5)
        except (OSError, subprocess.TimeoutExpired):
            pass


def run_gate(source: Path, target: str = "interpreter",
             timeout_s: int | None = None,
             skip_ra_return_cases: bool = False,
             accepted_boundary: str | None = None,
             harness_source: Path | None = None,
             build_root: Path | None = None) -> tuple[str, str]:
    """Run the cosim gate against one mutated source.

    Returns (verdict, detail). verdict is one of:
      "ok"           the gate exited zero and reported cosim: OK;
      "fail"         the gate exited nonzero and either reported cosim: FAIL with
                      a located divergence, or emitted the caller-authorized named
                      semantic boundary;
      "timeout"      the gate exceeded its runtime budget (never a kill);
      "inconsistent" the marker and the process outcome contradict each other;
      "no-run"       the harness never reported an outcome (build failure, crash,
                      or a failure with no located divergence).
    """
    env = dict(os.environ)
    env.pop("SR_COSIM_SKIP_AOT_RETURN_CASES", None)
    if skip_ra_return_cases:
        # These three unrelated transfer mutants intentionally alter the exact
        # control-flow/link premise the rewritten-$ra fixture relies on. Their
        # own existing cells remain active; the dedicated AOT-return mutant below
        # runs the new regression without this isolation.
        env["SR_COSIM_SKIP_AOT_RETURN_CASES"] = "1"
    if target == "generator":
        # A mutated copy of codegen.py still has to import its siblings
        # (analyze, host_stubs, build_profile), and running it from the ignored
        # build tree puts the wrong directory first on sys.path.
        env["PYTHONPATH"] = str(ROOT / "tools") + os.pathsep + env.get("PYTHONPATH", "")
    budget = GATE_TIMEOUT_S if timeout_s is None else timeout_s
    trees: dict[str, Path] = {}
    if harness_source is not None:
        trees["harness_source"] = harness_source
    if build_root is not None:
        trees["build_root"] = build_root
    command = make_command(source, target, **trees)
    try:
        process = subprocess.Popen(
            command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, errors="replace", env=env,
            # Own session on POSIX so a timeout reaps the whole build tree.
            start_new_session=(os.name != "nt"),
        )
    except OSError as exc:
        return "no-run", f"could not start the gate: {exc}"
    try:
        stdout, stderr = process.communicate(timeout=budget)
    except subprocess.TimeoutExpired:
        _terminate_process_tree(process)
        try:
            stdout, stderr = process.communicate(timeout=TERMINATE_GRACE_S)
        except (subprocess.TimeoutExpired, ValueError, OSError):
            stdout, stderr = "", ""
        return "timeout", (f"gate exceeded its {budget}s budget and was terminated; "
                           "a timeout is neither a pass nor a semantic kill")

    output = stdout + stderr
    detail = ""
    for index, line in enumerate(output.splitlines()):
        if line.startswith(("COSIM DIVERGENCE", "COSIM FPU REFERENCE FAIL")):
            detail = line.strip()
            remainder = output.splitlines()[index + 1 : index + 2]
            if remainder:
                detail += " | " + remainder[0].strip()
            break
    tail = "\n".join(output.strip().splitlines()[-6:])
    reported_ok = "cosim: OK" in output
    reported_fail = "cosim: FAIL" in output
    code = process.returncode

    if target == "scheduler":
        result_marker = "aot-return-boundary-selftest:"
        if code == 0 and result_marker in output and "0 failures" in output:
            return "ok", "aot-return-boundary-selftest passed"
        if (code != 0 and accepted_boundary and accepted_boundary in output and
                result_marker in output and "FAIL: AOT boundary isolation:" in output and
                "1 failures" in output):
            for line in output.splitlines():
                if line.startswith("SEMANTIC_BOUNDARY:") and accepted_boundary in line:
                    return "fail", line.strip()
        if code == 0:
            return "inconsistent", f"scheduler test exited 0 without a passing marker\n{tail}"
        return "no-run", tail

    if code != 0 and accepted_boundary:
        for line in output.splitlines():
            if line.startswith("SEMANTIC_BOUNDARY:") and accepted_boundary in line:
                return "fail", line.strip()

    if code == 0:
        if reported_ok:
            return "ok", detail
        if reported_fail:
            return "inconsistent", (f"gate exited 0 while reporting cosim: FAIL; "
                                    f"the marker contradicts the process outcome\n{tail}")
        return "no-run", tail
    if reported_ok:
        return "inconsistent", (f"gate exited {code} while reporting cosim: OK; "
                                "a nonzero process never passes on OK text\n" + tail)
    if reported_fail:
        if detail:
            return "fail", detail
        return "no-run", (f"gate exited {code} with cosim: FAIL but no divergence "
                          f"record was located; not a kill\n{tail}")
    return "no-run", tail


def gate_options(mutant: Mutant) -> dict[str, bool | str | None]:
    """The verdict rules one mutant's gate runs under."""
    skip_ra_return_cases = mutant.name in {
        "wrong-call-resume", "wrong-jump-target", "alter-link-value"
    }
    accepted_boundary = "AOT_RETURN_TARGET_MISMATCH" if mutant.name in {
        "wrong-call-resume", "alter-link-value"
    } else None
    if mutant.target == "scheduler":
        accepted_boundary = "AOT_RETURN_TARGET_MISMATCH"
    return {"skip_ra_return_cases": skip_ra_return_cases,
            "accepted_boundary": accepted_boundary}


BASELINES = {"interpreter": INTERPRETER, "scheduler": SCHEDULER}


def plan_lanes(mutants: tuple[Mutant, ...] | list[Mutant],
               jobs: int) -> list[tuple[str, list[int]]]:
    """Deal the campaign into lanes, each judged in a build root of its own.

    A gate is a serial build, so lanes run side by side. Cosimulation mutants are
    dealt round-robin over at most `jobs` lanes, so a lane's order, and the build
    history its tree sees, is fixed for a given `jobs`. Scheduler mutants build
    the HLE-thread harness, a different gate with its own baseline, so they form
    one lane of their own. Returns (baseline target, indices into `mutants`) for
    each lane: the cosimulation lanes, then the scheduler lane.
    """
    cosim = [index for index, mutant in enumerate(mutants) if mutant.target != "scheduler"]
    scheduler = [index for index, mutant in enumerate(mutants) if mutant.target == "scheduler"]
    lanes = [("interpreter", cosim[lane::jobs]) for lane in range(jobs)]
    lanes = [lane for lane in lanes if lane[1]]
    if scheduler:
        lanes.append(("scheduler", scheduler))
    return lanes


def run_lane(baseline_target: str, work: list[tuple[Mutant, Path, Path | None]],
             build_root: Path, baseline: Future, stop: threading.Event) -> list[tuple[str, str]]:
    """Run one lane: its baseline, then its mutants in order, all beneath `build_root`.

    The lane's tree starts empty and must pass the unmutated gate before it
    judges a mutant, so every tree that judges a mutant has passed the baseline.
    The baseline verdict is published through `baseline` as soon as it is
    known. A lane whose baseline fails, or that sees `stop`, runs no further
    mutant.
    """
    try:
        outcome = run_gate(BASELINES[baseline_target], baseline_target, build_root=build_root)
    except BaseException as error:
        baseline.set_exception(error)
        raise
    baseline.set_result(outcome)
    results: list[tuple[str, str]] = []
    if outcome[0] != "ok":
        return results
    for mutant, source, harness_source in work:
        if stop.is_set():
            break
        results.append(run_gate(
            source, mutant.target, harness_source=harness_source,
            build_root=build_root, **gate_options(mutant),
        ))
    return results


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build" / "cosim")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1,
                        help="parallel mutant lanes (default: one per CPU)")
    args = parser.parse_args(list(sys.argv[1:] if argv is None else argv))
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")

    sources = {
        "interpreter": INTERPRETER.read_text(encoding="utf-8"),
        "fp-convert": FP_CONVERT.read_text(encoding="utf-8"),
        "fpu-reference": FPU_REFERENCE.read_text(encoding="utf-8"),
        "generator": GENERATOR.read_text(encoding="utf-8"),
        "scheduler": SCHEDULER.read_text(encoding="utf-8"),
    }
    filenames = {
        "interpreter": "guest_interp.c",
        "fp-convert": "fp_convert.h",
        "fpu-reference": "fpu_reference.c",
        "generator": "codegen.py",
        "scheduler": "sched.c",
    }
    mutant_root = args.build_dir / "mutants"
    lane_root = args.build_dir / "lanes"
    for root in (mutant_root, lane_root):
        if root.exists():
            shutil.rmtree(root)
    mutant_root.mkdir(parents=True, exist_ok=True)

    # Every mutated source is written before any gate runs, so a mutant whose
    # pattern stopped matching fails the campaign before it spends a build.
    work: list[tuple[Mutant, Path, Path | None]] = []
    for mutant in MUTANTS:
        directory = mutant_root / mutant.name
        directory.mkdir(parents=True, exist_ok=True)
        target = directory / filenames[mutant.target]
        target.write_text(
            mutant.apply(sources[mutant.target]), encoding="utf-8", newline="\n"
        )
        harness_source = None
        if mutant.target == "scheduler":
            harness_source = directory / "hle_thread_selftest.c"
            shutil.copyfile(HLE_THREAD_SELFTEST, harness_source)
        if mutant.target == "fpu-reference":
            (directory / "fpu_reference.h").write_text(
                FPU_REFERENCE_HEADER.read_text(encoding="utf-8"),
                encoding="utf-8",
                newline="\n",
            )
        work.append((mutant, target, harness_source))

    # The campaign builds nothing outside lanes/: the developer's own trees are
    # neither mutated nor rebuilt, so nothing needs restoring afterwards.
    lanes = plan_lanes(MUTANTS, args.jobs)
    roots = [lane_root / f"{number}-{target}" for number, (target, _) in enumerate(lanes)]
    place: dict[int, tuple[int, int]] = {
        index: (number, position)
        for number, (_, indices) in enumerate(lanes)
        for position, index in enumerate(indices)
    }
    cosim_lanes = sum(1 for target, _ in lanes if target == "interpreter")
    print(f"COSIM_MUTATION lanes={len(lanes)} ({cosim_lanes} cosim, "
          f"{len(lanes) - cosim_lanes} scheduler)")
    print("COSIM_MUTATION baseline: the unmutated sources must pass in every lane")

    killed = 0
    survived: list[str] = []
    invalid: list[str] = []
    invalid_reasons: dict[str, list[str]] = {}
    stop = threading.Event()
    baselines: list[Future] = [Future() for _ in lanes]
    with ThreadPoolExecutor(max_workers=len(lanes)) as pool:
        futures = [
            pool.submit(run_lane, target, [work[index] for index in indices],
                        root, baseline, stop)
            for (target, indices), root, baseline in zip(lanes, roots, baselines, strict=True)
        ]
        failed = False
        for (target, _), root, baseline in zip(lanes, roots, baselines, strict=True):
            verdict, detail = baseline.result()
            if verdict != "ok":
                failed = True
                label = "baseline" if target == "interpreter" else "AOT boundary baseline"
                print(f"COSIM_MUTATION FAIL: {label} verdict={verdict} "
                      f"in {root.as_posix()}\n{detail}", file=sys.stderr)
        if failed:
            stop.set()
            return 1
        if cosim_lanes:
            print("COSIM_MUTATION baseline=OK")
        if cosim_lanes < len(lanes):
            print("COSIM_MUTATION AOT boundary baseline=OK")

        # Report in campaign order whatever order the lanes finish in.
        for index, mutant in enumerate(MUTANTS):
            number, position = place[index]
            verdict, detail = futures[number].result()[position]
            if verdict == "fail":
                killed += 1
                print(
                    f"COSIM_MUTATION KILLED  {mutant.name:<34} "
                    f"[{mutant.target}] {mutant.defect_class}"
                )
                if detail:
                    print(f"                       {detail}")
            elif verdict == "ok":
                survived.append(mutant.name)
                print(
                    f"COSIM_MUTATION SURVIVED {mutant.name:<34} "
                    f"[{mutant.target}] {mutant.defect_class}"
                )
            else:
                invalid.append(mutant.name)
                invalid_reasons.setdefault(verdict, []).append(mutant.name)
                print(f"COSIM_MUTATION INVALID  {mutant.name:<34} "
                      f"(verdict={verdict}; not a kill)")
                print(detail)

    by_target: dict[str, int] = {}
    for mutant in MUTANTS:
        by_target[mutant.target] = by_target.get(mutant.target, 0) + 1
    spread = ", ".join(f"{count} {name}" for name, count in sorted(by_target.items()))
    print(
        f"\nCOSIM_MUTATION summary: {killed}/{len(MUTANTS)} killed, "
        f"{len(survived)} survived, {len(invalid)} invalid ({spread})"
    )
    if survived:
        print("COSIM_MUTATION FAIL: the comparator did not detect: " + ", ".join(survived),
              file=sys.stderr)
    if invalid:
        for verdict, names in sorted(invalid_reasons.items()):
            print(f"COSIM_MUTATION FAIL: {len(names)} mutant(s) had verdict "
                  f"{verdict} (neither a pass nor a kill): " + ", ".join(names),
                  file=sys.stderr)
    return 1 if (survived or invalid) else 0


if __name__ == "__main__":
    raise SystemExit(main())
