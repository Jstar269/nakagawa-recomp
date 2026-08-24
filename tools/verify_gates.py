#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Run optional differential verification gates without shell-specific syntax."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys

import build_profile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", required=True)
    parser.add_argument("--elf", default="")
    parser.add_argument("--env-elf", action="store_true")
    parser.add_argument("--run-elf", required=True)
    parser.add_argument("--workdir", required=True)
    parser.add_argument("--codegen-oracle", default="")
    parser.add_argument("--microtest-module", default="")
    parser.add_argument("--microtest-oracle", default="")
    args = parser.parse_args()

    try:
        elf = build_profile.resolve_path(
            "GAME_ELF",
            cli_value=args.elf or None,
            use_env=args.env_elf,
            cli_label="--elf option", flag="--env-elf",
            # The gates below are BLOCKED without external oracle traces, so this
            # entry point must still report cleanly when no ELF is on disk yet.
            must_exist=False,
        )
    except build_profile.BuildInputError as exc:
        sys.stderr.write(f"verify_gates: {exc}\n")
        return 2

    repo = Path(__file__).resolve().parent.parent
    workdir = Path(args.workdir)
    status = 0

    print("[verify] codegen_gate: <elf> <oracle.trace> <workdir>", flush=True)
    if args.codegen_oracle:
        env = {**os.environ, "CC": args.cc}
        result = subprocess.run(
            [
                sys.executable,
                str(repo / "tools" / "codegen_gate.py"),
                elf,
                args.codegen_oracle,
                str(workdir / "codegen"),
            ],
            cwd=repo,
            env=env,
            check=False,
        )
        status |= result.returncode != 0
    else:
        print(
            f"  BLOCKED: CODEGEN_ORACLE not set (need a PPSSPP-captured .trace for {elf})",
            flush=True,
        )
        status = 1

    print("[verify] microtest_gate: <run_elf.exe> <module.elf> <oracle.trace> <workdir>", flush=True)
    if args.microtest_module and args.microtest_oracle:
        result = subprocess.run(
            [
                sys.executable,
                str(repo / "tools" / "microtest_gate.py"),
                args.run_elf,
                args.microtest_module,
                args.microtest_oracle,
                str(workdir / "microtest"),
            ],
            cwd=repo,
            check=False,
        )
        status |= result.returncode != 0
    else:
        print(
            "  BLOCKED: MICROTEST_MODULE and/or MICROTEST_ORACLE not set "
            "(need a PSP-compiled microtest .elf + PPSSPP .trace)",
            flush=True,
        )
        status = 1

    print("[verify] done.", flush=True)
    return int(status != 0)


if __name__ == "__main__":
    raise SystemExit(main())
