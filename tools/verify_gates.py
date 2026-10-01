#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Run optional differential verification gates without shell-specific syntax."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys

import build_profile
import tracediff


_LEGACY_V1_TIER = "LEGACY_V1"


def _declares_v2_header(path: str) -> bool:
    """Whether the first comment line claims the v2 format (valid or not).

    Everything else is read the way the v1 comparator reads it: its header is
    whatever the first comment is and its step lines are what matter, so a stream
    that does not claim v2 stays usable by the non-hardware gates. A stream that
    claims v2 but fails validation never degrades to legacy.
    """

    with open(path, "r", encoding="utf-8") as handle:
        for raw in handle:
            line = raw.rstrip("\r\n")
            if not line.strip():
                continue
            if line.startswith("#"):
                return line[1:].split()[:3] == ["psp-recomp", "trace", "v2"]
            return False
    return False


def trace_source_tier(path: str) -> str:
    """Read the evidence tier from the trace header (cheap, header-only)."""

    try:
        return tracediff.read_source_tier(path)
    except tracediff.HardwareTraceError:
        if _declares_v2_header(path):
            raise
        return _LEGACY_V1_TIER


def report_trace_tier(label: str, path: str, *, hardware_gate: bool = False) -> str:
    tier = trace_source_tier(path)
    if tier == _LEGACY_V1_TIER:
        print(f"  CORROBORATIVE_ONLY: {label} uses a legacy v1 trace", flush=True)
    elif tier == "PPSSPP_CORROBORATIVE":
        print(
            f"  CORROBORATIVE_ONLY: {label} source_tier={tier}",
            flush=True,
        )
    else:
        context = "" if hardware_gate else " (non-hardware gate)"
        print(f"  TRACE_TIER: {label} source_tier={tier}{context}", flush=True)
    return tier


def _trace_error_detail(exc: Exception) -> str:
    if isinstance(exc, tracediff.HardwareTraceError):
        return str(exc).rsplit(": ", 1)[-1]
    return "trace input could not be read or validated"


def run_hardware_trace_gate(psp_trace: str, cosim_trace: str) -> int:
    print("[verify] hardware_trace_gate: PSP_HARDWARE_TRACE + LOCAL_COSIM_TRACE", flush=True)
    if not psp_trace or not cosim_trace:
        print(
            "  NOT_RUN: set both PSP_HARDWARE_TRACE and LOCAL_COSIM_TRACE",
            flush=True,
        )
        print(
            "  PSP_HARDWARE_TRACE producer: in the works (issue #312)",
            flush=True,
        )
        return 1

    try:
        psp_tier = report_trace_tier("PSP_HARDWARE_TRACE", psp_trace, hardware_gate=True)
        cosim_tier = report_trace_tier("LOCAL_COSIM_TRACE", cosim_trace, hardware_gate=True)
    except (tracediff.HardwareTraceError, OSError, UnicodeDecodeError) as exc:
        print(
            f"  REJECTED: {_trace_error_detail(exc)}",
            flush=True,
        )
        return 1

    if psp_tier in (_LEGACY_V1_TIER, "PPSSPP_CORROBORATIVE") or cosim_tier in (
        _LEGACY_V1_TIER,
        "PPSSPP_CORROBORATIVE",
    ):
        print(
            "  CORROBORATIVE_ONLY: v1 and PPSSPP_CORROBORATIVE traces cannot satisfy the hardware gate",
            flush=True,
        )
        return 1
    if psp_tier != "PSP_HARDWARE" or cosim_tier != "LOCAL_COSIM":
        print(
            "  NOT_RUN: hardware gate requires PSP_HARDWARE + LOCAL_COSIM traces",
            flush=True,
        )
        return 1

    try:
        divergence = tracediff.strict_hardware_diff(psp_trace, cosim_trace)
    except (tracediff.HardwareTraceError, OSError, UnicodeDecodeError) as exc:
        print(
            f"  REJECTED: {_trace_error_detail(exc)}",
            flush=True,
        )
        return 1
    if divergence is None:
        print(
            "  STRICT_V2_AGREEMENT: PSP_HARDWARE == LOCAL_COSIM under strict v2 comparison "
            "(tiers come from trace metadata; not device-attested, not a hardware measurement)",
            flush=True,
        )
        return 0
    step, pc, detail = divergence
    print(f"  FAIL: divergence at step {step}, pc {pc}: {detail}", flush=True)
    return 1


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
    parser.add_argument("--psp-hardware-trace", default="")
    parser.add_argument("--local-cosim-trace", default="")
    args = parser.parse_args()

    try:
        elf = build_profile.resolve_path(
            "GAME_ELF",
            cli_value=args.elf or None,
            use_env=args.env_elf,
            cli_label="--elf option", flag="--env-elf",
            # The gates below are NOT_RUN without external trace inputs, so this
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
        try:
            report_trace_tier("CODEGEN_ORACLE", args.codegen_oracle)
        except (tracediff.HardwareTraceError, OSError, UnicodeDecodeError) as exc:
            print(f"  BLOCKED: CODEGEN_ORACLE tier unavailable: {_trace_error_detail(exc)}", flush=True)
            status = 1
        else:
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
            f"  NOT_RUN: CODEGEN_ORACLE not set (provide an external trace for {elf})",
            flush=True,
        )
        status = 1

    print("[verify] microtest_gate: <run_elf.exe> <module.elf> <oracle.trace> <workdir>", flush=True)
    if args.microtest_module and args.microtest_oracle:
        try:
            report_trace_tier("MICROTEST_ORACLE", args.microtest_oracle)
        except (tracediff.HardwareTraceError, OSError, UnicodeDecodeError) as exc:
            print(f"  BLOCKED: MICROTEST_ORACLE tier unavailable: {_trace_error_detail(exc)}", flush=True)
            status = 1
        else:
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
            "  NOT_RUN: MICROTEST_MODULE and/or MICROTEST_ORACLE not set "
            "(provide a PSP-compiled microtest .elf and an external trace)",
            flush=True,
        )
        status = 1

    if args.psp_hardware_trace or args.local_cosim_trace:
        status |= run_hardware_trace_gate(
            args.psp_hardware_trace,
            args.local_cosim_trace,
        ) != 0
    else:
        # The hardware pair is an opt-in additional gate: leaving it out must not
        # make the existing codegen/microtest route unable to pass. It is still
        # reported, never silent.
        print("[verify] hardware_trace_gate: PSP_HARDWARE_TRACE + LOCAL_COSIM_TRACE", flush=True)
        print(
            "  NOT_RUN: optional gate; set both PSP_HARDWARE_TRACE and LOCAL_COSIM_TRACE to run it",
            flush=True,
        )
        print("  PSP_HARDWARE_TRACE producer: in the works (issue #312)", flush=True)

    print("[verify] done.", flush=True)
    return int(status != 0)


if __name__ == "__main__":
    raise SystemExit(main())
