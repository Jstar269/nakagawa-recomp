# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Prove the AOT/interpreter cosimulation gate is load-bearing.

A comparator that cannot fail proves nothing.  This driver takes the production
interpreter, applies one semantic defect at a time to a COPY of it under the
ignored build tree, rebuilds the cosim harness against that copy and requires the
gate to FAIL.

Two rules make the campaign meaningful rather than decorative:

* A mutant must BUILD.  A compile error is not a kill -- it proves the compiler
  noticed, not the comparator -- so a mutant whose harness never ran is reported
  as INVALID and fails the campaign.
* A mutant must change behavior the comparator claims to cover.  Each entry below
  names the mission-level defect class it stands for, and the driver records the
  first divergence the gate actually reported, so a mutant that fails "for some
  other reason" is visible in the output rather than counted silently.

The tree is never modified: mutants are written to
``<build-dir>/mutants/<name>/guest_interp.c`` and selected through the Makefile's
``COSIM_INTERP_SRC`` override.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
INTERPRETER = ROOT / "src" / "rt" / "guest_interp.c"


class Mutant:
    """One semantic defect, expressed as exact text substitutions.

    Every substitution declares how many occurrences it must replace.  A
    substitution that stops matching -- because the interpreter was refactored --
    is a hard error, not a silently skipped mutant: that is exactly how a
    mutation campaign rots into a no-op.
    """

    def __init__(self, name: str, defect_class: str, substitutions: list[tuple[str, str, int]]):
        self.name = name
        self.defect_class = defect_class
        self.substitutions = substitutions

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
            # has nothing to do with the comparator. Remove the whole mechanism.
            ("s->r[0] = 0u;", "(void)0;", 7),
        ],
    ),
    Mutant(
        "wrong-branch-target",
        "incorrect branch target",
        [(
            "        out->target = pc + 4u + (sign_extend_16(opcode) << 2);",
            "        out->target = pc + 8u + (sign_extend_16(opcode) << 2);",
            1,
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
            "        if (sr_lookup(pc)) {\n            s->pc = pc;",
            "        if (sr_lookup(pc)) {\n            s->pc = pc + 4u;",
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
        "drop-fcr31-threading",
        "guest FCR31 not threaded into the scalar FPU helper path",
        [(
            "            s->fi[fd] = sr_fpu_to_word(s->f[fs], funct, s->fcr31);",
            "            s->fi[fd] = sr_fpu_to_word(s->f[fs], funct, 0u);",
            1,
        )],
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


def make_command(interpreter_source: Path) -> list[str]:
    return [
        resolve_make(),
        "--no-print-directory",
        "cosim-selftest",
        f"COSIM_INTERP_SRC={interpreter_source.as_posix()}",
    ]


def run_gate(interpreter_source: Path) -> tuple[str, str]:
    """Run the cosim gate against one interpreter source.

    Returns (verdict, detail) where verdict is one of "ok", "fail" or "no-run".
    "no-run" means the harness binary never printed its result -- the build
    failed, or the run died before reporting -- which is NOT a mutation kill.
    """
    completed = subprocess.run(
        make_command(interpreter_source),
        cwd=ROOT,
        capture_output=True,
        text=True,
        errors="replace",
    )
    output = completed.stdout + completed.stderr
    detail = ""
    for index, line in enumerate(output.splitlines()):
        if line.startswith("COSIM DIVERGENCE"):
            detail = line.strip()
            remainder = output.splitlines()[index + 1 : index + 2]
            if remainder:
                detail += " | " + remainder[0].strip()
            break
    if "cosim: FAIL" in output:
        return "fail", detail
    if "cosim: OK" in output:
        return "ok", detail
    tail = "\n".join(output.strip().splitlines()[-6:])
    return "no-run", tail


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build" / "cosim")
    args = parser.parse_args(list(sys.argv[1:] if argv is None else argv))

    source = INTERPRETER.read_text(encoding="utf-8")
    mutant_root = args.build_dir / "mutants"
    if mutant_root.exists():
        shutil.rmtree(mutant_root)
    mutant_root.mkdir(parents=True, exist_ok=True)

    print("COSIM_MUTATION baseline: the unmutated interpreter must pass")
    verdict, detail = run_gate(INTERPRETER)
    if verdict != "ok":
        print(f"COSIM_MUTATION FAIL: baseline verdict={verdict}\n{detail}", file=sys.stderr)
        return 1
    print("COSIM_MUTATION baseline=OK")

    killed = 0
    survived: list[str] = []
    invalid: list[str] = []
    for mutant in MUTANTS:
        directory = mutant_root / mutant.name
        directory.mkdir(parents=True, exist_ok=True)
        target = directory / "guest_interp.c"
        target.write_text(mutant.apply(source), encoding="utf-8", newline="\n")

        verdict, detail = run_gate(target)
        if verdict == "fail":
            killed += 1
            print(f"COSIM_MUTATION KILLED  {mutant.name:<24} [{mutant.defect_class}]")
            if detail:
                print(f"                       {detail}")
        elif verdict == "ok":
            survived.append(mutant.name)
            print(f"COSIM_MUTATION SURVIVED {mutant.name:<24} [{mutant.defect_class}]")
        else:
            invalid.append(mutant.name)
            print(f"COSIM_MUTATION INVALID  {mutant.name:<24} (never ran; not a kill)")
            print(detail)

    # Leave the tree in the state a reader expects: the real interpreter, passing.
    print("COSIM_MUTATION restoring the unmutated build")
    verdict, detail = run_gate(INTERPRETER)
    if verdict != "ok":
        print(f"COSIM_MUTATION FAIL: restored baseline verdict={verdict}\n{detail}",
              file=sys.stderr)
        return 1

    print(
        f"\nCOSIM_MUTATION summary: {killed}/{len(MUTANTS)} killed, "
        f"{len(survived)} survived, {len(invalid)} invalid"
    )
    if survived:
        print("COSIM_MUTATION FAIL: the comparator did not detect: " + ", ".join(survived),
              file=sys.stderr)
    if invalid:
        print("COSIM_MUTATION FAIL: mutants that never executed: " + ", ".join(invalid),
              file=sys.stderr)
    return 1 if (survived or invalid) else 0


if __name__ == "__main__":
    raise SystemExit(main())
