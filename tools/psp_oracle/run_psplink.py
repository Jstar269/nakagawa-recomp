# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Bounded PC-side PSPLINK runner and result comparator.

The USB/driver and PSPLink launch command are intentionally explicit.  This
tool never installs drivers, flashes firmware, starts a shell, or assumes that
process exit means that a probe passed.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import json
import re
import shlex
import shutil
import subprocess
import sys
import time

try:
    from .protocol import compare_texts, dump_json
except ImportError:  # direct ``python tools/psp_oracle/run_psplink.py`` invocation
    from protocol import compare_texts, dump_json


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_RESULTS = ROOT / "oracle" / "hardware-results"
TERMINAL_OUTCOMES = frozenset({"HANG", "RESET"})
_TEST_RECORD_RE = re.compile(
    r"^(?:host0:/>\s*)?NAKAGAWA_PSP_TEST\b.*\bstatus=([A-Z]+)\b", re.MULTILINE
)


def _tool(name: str) -> str | None:
    resolved = shutil.which(name)
    return Path(resolved).name if resolved else None


def _plan(args: argparse.Namespace) -> dict[str, object]:
    prx = Path(args.prx).resolve() if args.prx else None
    return {
        "schema": 1,
        "mode": "dry-run",
        "pspsh": _tool(args.pspsh),
        "usbhostfs_pc": _tool("usbhostfs_pc"),
        "prx": prx.name if prx else None,
        "remote_command": args.remote_command,
        "results_directory": str(DEFAULT_RESULTS.relative_to(ROOT)).replace("\\", "/"),
        "provenance_supplied": all(getattr(args, flag) for flag in PROVENANCE_FLAGS),
        "manual_steps_remaining": [
            "connect a human-configured PSPLink session",
            "confirm host0 round-trip before launching a probe",
            "supply --binary/--source-commit/--model/--firmware so the capture is acceptance-eligible",
        ],
    }


def _run_command(command: list[str], timeout: float) -> tuple[int | None, str, str, str]:
    started = time.monotonic()
    try:
        completed = subprocess.run(
            command,
            stdin=None,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            check=False,
            shell=False,
        )
    except subprocess.TimeoutExpired as exc:
        return None, (exc.stdout or "")[-65536:], (exc.stderr or "")[-65536:], "TIMEOUT"
    except OSError as exc:
        return None, "", type(exc).__name__, "ERROR"
    status = "PROCESS_EXITED"
    return completed.returncode, completed.stdout[-65536:], completed.stderr[-65536:], status


def _split_command(command: str) -> list[str]:
    """Split an explicit Windows command while removing one quoting layer.

    ``shlex.split(..., posix=False)`` preserves the quotes that protect a
    multi-word ``pspsh -e`` payload.  ``subprocess.run(shell=False)`` then
    passes those quotes literally, and PSPLINK treats the whole payload as a
    filename.  Strip only a matching outer pair; embedded quotes and Windows
    backslashes remain untouched.
    """

    tokens = shlex.split(command, posix=False)
    cleaned: list[str] = []
    for token in tokens:
        if len(token) >= 2 and token[0] == token[-1] and token[0] in {"'", '"'}:
            token = token[1:-1]
        cleaned.append(token)
    return cleaned


def _record_summary(text: str) -> tuple[str, int]:
    statuses = _TEST_RECORD_RE.findall(text)
    if not statuses:
        return "NO_RECORD", 0
    unique = set(statuses)
    if unique == {"SKIP"}:
        return "SKIP_RECORDS", len(statuses)
    if "SKIP" in unique:
        return "MIXED_RECORDS", len(statuses)
    return "RESULT_RECORDS", len(statuses)


def annotate_terminal_outcome(
    report: dict[str, object], capture: bytes, outcome: str
) -> dict[str, object]:
    """Attach a human-observed HANG/RESET label to a no-record capture.

    A host process exit cannot distinguish a device reset from a probe hang.
    The annotation is therefore explicit human evidence, never an inference,
    and it is rejected when the PSP already emitted any scalar test record.
    """

    if outcome not in TERMINAL_OUTCOMES:
        raise ValueError(f"unsupported terminal outcome: {outcome}")
    text = capture.decode("utf-8", errors="replace")
    classification, record_count = _record_summary(text)
    if record_count:
        raise ValueError("terminal outcome annotation requires a capture with no test records")
    annotated = dict(report)
    annotated["record_classification"] = classification
    annotated["test_record_count"] = 0
    annotated["terminal_outcome"] = outcome
    annotated["terminal_outcome_source"] = "human-observed"
    annotated["capture_sha256"] = hashlib.sha256(capture).hexdigest()
    annotated["acceptance_eligible"] = False
    annotated["acceptance_blockers"] = [
        "terminal outcome has no scalar PSP result stream",
        "human observation must be accompanied by model/firmware/CFW/clock in the hardware handoff",
    ]
    return annotated


PROVENANCE_FLAGS = ("binary", "source_commit", "model", "firmware")


def _canonicalize_psp(text: str, args: argparse.Namespace) -> str:
    """Replace fixture placeholders with host-measured provenance metadata.

    When no provenance is supplied the fixture placeholders are left in place on
    purpose: the comparison then reports ``acceptance_eligible: false`` rather
    than silently looking like a measured hardware result.
    """

    if not any(getattr(args, flag) for flag in PROVENANCE_FLAGS):
        return text
    digest = hashlib.sha256()
    with args.binary.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    metadata = (
        "NAKAGAWA_PSP_META schema=1 source=psp "
        f"model={args.model} firmware={args.firmware} "
        f"binary_sha256={digest.hexdigest()} source_commit={args.source_commit}"
    )
    records = [
        line for line in text.splitlines()
        if not line.lstrip().startswith("NAKAGAWA_PSP_META ")
        and not line.lstrip().startswith("host0:/> NAKAGAWA_PSP_META ")
    ]
    return metadata + "\n" + "\n".join(records) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--pspsh", default="pspsh", help="explicit pspsh executable name/path")
    parser.add_argument("--prx", help="source-owned PRX to launch")
    parser.add_argument("--remote-command", help="explicit pspsh command accepted by the installed build")
    parser.add_argument("--command", help="host command to run for capture (no shell expansion)")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--psp-output", type=Path)
    parser.add_argument("--nakagawa-output", type=Path)
    parser.add_argument("--binary", type=Path, help="source-owned PRX used to replace fixture metadata")
    parser.add_argument("--source-commit", help="exact source commit recorded in the result metadata")
    parser.add_argument("--model", help="human-recorded PSP model identifier")
    parser.add_argument("--firmware", help="human-recorded PSP firmware identifier")
    parser.add_argument("--out", type=Path)
    parser.add_argument(
        "--annotate-report",
        type=Path,
        help="existing capture report to annotate after a human-observed no-record outcome",
    )
    parser.add_argument(
        "--observed-terminal-outcome",
        choices=sorted(TERMINAL_OUTCOMES),
        help="human-observed HANG or RESET; never inferred from host process status",
    )
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    supplied = [flag for flag in PROVENANCE_FLAGS if getattr(args, flag)]
    if supplied and len(supplied) != len(PROVENANCE_FLAGS):
        missing = ", ".join("--" + flag.replace("_", "-") for flag in PROVENANCE_FLAGS if flag not in supplied)
        parser.error(f"provenance metadata is all-or-nothing; missing {missing}")

    if bool(args.annotate_report) != bool(args.observed_terminal_outcome):
        parser.error("--annotate-report and --observed-terminal-outcome must be supplied together")
    if args.annotate_report:
        if args.command or args.psp_output or args.nakagawa_output or args.dry_run:
            parser.error("terminal annotation cannot launch or compare another capture")
        report = json.loads(args.annotate_report.read_text(encoding="utf-8"))
        stdout_file = report.get("stdout_file")
        if not isinstance(stdout_file, str):
            parser.error("capture report has no stdout_file")
        capture = (ROOT / stdout_file).resolve()
        try:
            capture.relative_to(DEFAULT_RESULTS.resolve())
        except ValueError:
            parser.error("capture report stdout_file escapes the hardware-results directory")
        if not capture.is_file():
            parser.error("capture report stdout_file does not exist")
        try:
            annotated = annotate_terminal_outcome(
                report, capture.read_bytes(), args.observed_terminal_outcome
            )
        except ValueError as exc:
            parser.error(str(exc))
        rendered = dump_json(annotated)
        if args.out:
            if args.out.resolve() == args.annotate_report.resolve():
                parser.error("refusing to overwrite the original capture report")
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(rendered, encoding="utf-8")
        else:
            sys.stdout.write(rendered)
        return 0

    if args.dry_run or not args.command:
        report = _plan(args)
        rendered = dump_json(report)
        if args.out:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(rendered, encoding="utf-8")
        else:
            sys.stdout.write(rendered)
        return 0

    command = _split_command(args.command)
    if not command:
        parser.error("--command must contain an executable")
    returncode, stdout, stderr, process_status = _run_command(command, args.timeout)
    DEFAULT_RESULTS.mkdir(parents=True, exist_ok=True)
    timestamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    capture = DEFAULT_RESULTS / f"psplink-{timestamp}.stdout.txt"
    capture.write_text(stdout, encoding="utf-8")
    report: dict[str, object] = {
        "schema": 1,
        "mode": "capture",
        "process_status": process_status,
        "returncode": returncode,
        "stdout_file": str(capture.relative_to(ROOT)).replace("\\", "/"),
        "stderr_present": bool(stderr),
    }
    record_classification, record_count = _record_summary(stdout)
    report["record_classification"] = record_classification
    report["test_record_count"] = record_count
    if args.psp_output and args.nakagawa_output:
        report["comparison"] = compare_texts(
            _canonicalize_psp(args.psp_output.read_text(encoding="utf-8"), args),
            args.nakagawa_output.read_text(encoding="utf-8"),
        )
    elif args.psp_output or args.nakagawa_output:
        report["comparison"] = {
            "classification": "INCONCLUSIVE",
            "error": "both --psp-output and --nakagawa-output are required",
        }
    rendered = dump_json(report)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(rendered, encoding="utf-8")
    else:
        sys.stdout.write(rendered)
    return 0 if process_status == "PROCESS_EXITED" and returncode == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
