# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Bounded PC-side PSPLINK runner and result comparator.

The USB/driver and PSPLink launch command are intentionally explicit.  This
tool never installs drivers, flashes firmware, starts a shell, or assumes that
process exit means that a probe passed.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import math
import os
from pathlib import Path
import json
import re
import shlex
import shutil
import stat
import subprocess
import sys
import threading
import time
import traceback
from typing import Callable

_PACKAGE_PARENT = str(Path(__file__).resolve().parents[1])
if _PACKAGE_PARENT not in sys.path:
    sys.path.insert(0, _PACKAGE_PARENT)

try:
    from .protocol import (
        ParsedOutput,
        ProtocolError,
        compare_texts,
        decode_psp_model_code,
        dump_json,
        ge_corpus_report,
        model_identity_fields,
        parse_output,
        parse_progress,
        provenance_issues,
        validate_dmac_size_matrix,
        validate_dmac_size_matrix_size,
    )
except ImportError:  # direct ``python tools/psp_oracle/run_psplink.py`` invocation
    from psp_oracle.protocol import (
        ParsedOutput,
        ProtocolError,
        compare_texts,
        decode_psp_model_code,
        dump_json,
        ge_corpus_report,
        model_identity_fields,
        parse_output,
        parse_progress,
        provenance_issues,
        validate_dmac_size_matrix,
        validate_dmac_size_matrix_size,
    )

try:
    from .parse_golden import (
        CAMPAIGN_PROBE_CASES,
        DMAC_INVALID_CASES,
        parse_cache_alias_output,
        parse_campaign_probe_output,
        parse_audio_query_output,
        parse_delay_zero_output,
        parse_dmac_cells_output,
        parse_dmac_invalid_tail_output,
        parse_fpu_vector_output,
        parse_ge_nan_output,
        parse_io_matrix_output,
        parse_mbx_delete_wait_output,
        parse_registry_readonly_output,
        validate_hle_edram_restore,
    )
except ImportError:  # direct ``python tools/psp_oracle/run_psplink.py`` invocation
    from psp_oracle.parse_golden import (
        CAMPAIGN_PROBE_CASES,
        DMAC_INVALID_CASES,
        parse_cache_alias_output,
        parse_campaign_probe_output,
        parse_audio_query_output,
        parse_delay_zero_output,
        parse_dmac_cells_output,
        parse_dmac_invalid_tail_output,
        parse_fpu_vector_output,
        parse_ge_nan_output,
        parse_io_matrix_output,
        parse_mbx_delete_wait_output,
        parse_registry_readonly_output,
        validate_hle_edram_restore,
    )


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_RESULTS = ROOT / "oracle" / "hardware-results"
DEFAULT_GE_CORPUS = ROOT / "fixtures" / "psp_oracle" / "ge_corpus.json"
DEFAULT_GE_CORPUS_SCHEMA = ROOT / "assets" / "ge_corpus.schema.json"
TERMINAL_OUTCOMES = frozenset({"HANG", "RESET"})
_TEST_RECORD_RE = re.compile(
    r"^NAKAGAWA_PSP_TEST\b.*\bstatus=([A-Z]+)\b", re.MULTILINE
)
SHELL_VERIFICATION_ATTEMPTS = 3
SHELL_VERIFICATION_ATTEMPT_TIMEOUT = 15.0
DEFAULT_SHELL_VERIFICATION_TIMEOUT = 45.0
HOST0_MTIME_TOLERANCE_NS = 1_000_000_000
_FULL_COMMIT_RE = re.compile(r"(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})")
# PSPLink `pspver` prints the firmware as dot-separated decimal fields, for
# example `Version: 6.6.1 (0x06060110)` for firmware 6.61. An expected firmware
# is compared with that string exactly, so it must use the same form.
_PSPVER_FIRMWARE_RE = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")


def _expected_firmware_problem(value: object) -> str | None:
    """Explain why an expected firmware cannot match `pspver`, or return None."""

    if value is None or (isinstance(value, str) and _PSPVER_FIRMWARE_RE.fullmatch(value)):
        return None
    return (
        "expected firmware must use the PSPLink `pspver` form <major>.<minor>.<patch> "
        f"(for example 6.6.1 for firmware 6.61), got {value!r}"
    )
_FULL_SHA256_RE = re.compile(r"[0-9a-fA-F]{64}")
_ALL_ZERO_RE = re.compile(r"0+")


def _fixed_campaign_rows(test_id: str, cases: list[tuple[str, int, frozenset[str]]]):
    return test_id, tuple(cases)


_PASS = frozenset({"PASS"})
_PASS_OR_SKIP = frozenset({"PASS", "SKIP"})
_FIXED_CAMPAIGN_CASES = {
    "smoke": _fixed_campaign_rows(
        "PSP-SMOKE-001", [("sum-1-to-100", 1, _PASS)]
    ),
    "thread-exit-delete": _fixed_campaign_rows(
        "PSP-THREAD-EXIT-001",
        [(case_id, 6, _PASS) for case_id in (
            "ED-R77", "ED-R00", "ED-RNEG", "ED-RERR",
            "ED-X77", "ED-X00", "ED-XNEG", "ED-XERR",
            "ED-D77", "ED-D00", "ED-DNEG", "ED-DERR",
        )],
    ),
    "teardown-test": _fixed_campaign_rows(
        "PSP-TEARDOWN-001", [("exitdelete-main", 1, frozenset({"SKIP"}))]
    ),
    "display-mask-duty": _fixed_campaign_rows(
        "PSP-DISPLAY-001",
        [(f"display-mask-duty-{duration}us", 13, _PASS)
         for duration in (33000, 66000)],
    ),
    # run_display_wait_late emits, per API, its five late cells and then that
    # API's in-vblank cell (PASS, or SKIP when the in-vblank phase was not
    # reached), after one calibration record.
    "display-wait-late": _fixed_campaign_rows(
        "PSP-DISPLAY-002",
        [("calibration", 5, _PASS)]
        + [
            row
            for api in ("waitvblankstart", "waitvblank")
            for row in (
                [(f"late-{api}-{offset}eighths", 22, _PASS) for offset in (2, 6, 10, 14, 20)]
                + [(f"invblank-{api}", 22, _PASS_OR_SKIP)]
            )
        ],
    ),
    "display-wait-priority": _fixed_campaign_rows(
        "PSP-DISPLAY-003",
        [("calibration", 5, _PASS),
         ("priority-control", 16, _PASS),
         ("priority-experiment", 16, _PASS)],
    ),
    "display-vblank-window": _fixed_campaign_rows(
        "PSP-DISPLAY-004", [("vblank-window", 14, _PASS)]
    ),
    "mutex-refer-unlocked": _fixed_campaign_rows(
        "PSP-MUTEX-001", [("mutex-refer-unlocked", 5, _PASS)]
    ),
    "mutex-timeout-quanta": _fixed_campaign_rows(
        "PSP-MUTEX-001", [("mutex-timeout-quanta", 17, _PASS)]
    ),
    "mutex-priority-inheritance": _fixed_campaign_rows(
        "PSP-MUTEX-001", [("mutex-priority-inheritance", 7, _PASS)]
    ),
    # Each trial records sceKernelIsCpuIntrEnable() from the VBLANK handler in
    # out0 and the six mutex returns in out1..out6. By the probe's design a
    # trial counts (PASS) only when out0 is 0; FAIL marks a trial that does not
    # count. _check_mutex_interrupt_trial enforces that pairing exactly.
    "mutex-interrupt-context": _fixed_campaign_rows(
        "PSP-MUTEX-001",
        [("mutex-interrupt-context", 3, _PASS)]
        + [(f"mutex-interrupt-context-t{index:02d}", 7, frozenset({"PASS", "FAIL"}))
           for index in range(20)],
    ),
}


def _check_mutex_interrupt_trial(record) -> None:
    """A trial's status must follow its recorded interrupt state, nothing else."""

    if not record.case_id.startswith("mutex-interrupt-context-t"):
        return
    values = dict(record.values)
    interrupts_enabled = int(values["out0"], 0)
    expected = "PASS" if interrupts_enabled == 0 else "FAIL"
    if record.status != expected or int(values["result"], 0) != (expected == "PASS"):
        raise ProtocolError(
            f"mutex-interrupt-context: {record.case_id} status {record.status} "
            f"disagrees with its interrupt state out0={values['out0']}"
        )


_FIXED_CAMPAIGN_RECORD_CHECKS = {
    "mutex-interrupt-context": _check_mutex_interrupt_trial,
}


CAMPAIGN_QUEUE_CASES = (
    "transport-write",
    "kernel-alarm",
    "thread-scheduler",
    "wait-outcomes",
    "ge-break-continue",
    "refer-status-size",
    "registry-readonly",
    "llsc-link",
    "kernel-misc",
    "smoke",
    "thread-exit-delete",
    "teardown-test",
    "io-matrix",
    "display-mask-duty",
    "display-wait-late",
    "display-wait-priority",
    "display-vblank-window",
    "fpu-vector",
    "cache-alias",
    "audio-query",
    "ge-nan",
    "dma-cells",
    "dma-invalid-tail-s0",
    "dma-invalid-tail-memcpy-dst",
    "dma-invalid-tail-memcpy-src",
    "dma-invalid-tail-try-dst",
    "dma-invalid-tail-try-src",
    "mutex-refer-unlocked",
    "mutex-timeout-quanta",
    "mutex-priority-inheritance",
    "mutex-interrupt-context",
)

CAMPAIGN_CASE_ESTIMATE_SECONDS = {
    "transport-write": 90,
    "kernel-alarm": 120,
    "thread-scheduler": 90,
    "wait-outcomes": 90,
    "ge-break-continue": 180,
    "refer-status-size": 120,
    "registry-readonly": 240,
    "llsc-link": 90,
    "kernel-misc": 90,
    "smoke": 60,
    "thread-exit-delete": 180,
    "teardown-test": 60,
    "io-matrix": 180,
    "display-mask-duty": 300,
    "display-wait-late": 240,
    "display-wait-priority": 180,
    "display-vblank-window": 180,
    "fpu-vector": 90,
    "cache-alias": 90,
    "audio-query": 180,
    "ge-nan": 240,
    "dma-cells": 180,
    "dma-invalid-tail-s0": 120,
    "dma-invalid-tail-memcpy-dst": 120,
    "dma-invalid-tail-memcpy-src": 120,
    "dma-invalid-tail-try-dst": 120,
    "dma-invalid-tail-try-src": 120,
    "mutex-refer-unlocked": 90,
    "mutex-timeout-quanta": 120,
    "mutex-priority-inheritance": 120,
    "mutex-interrupt-context": 120,
}


class UnsafeHost0OutputError(OSError):
    """A host0 result path is not a regular file owned by the scratch root."""


@dataclass(frozen=True)
class PsplinkSnapshot:
    threads: frozenset[tuple[str, str]]
    memory_free_bytes: tuple[tuple[int, tuple[int, int]], ...]
    modules: frozenset[tuple[str, str]]


_THREAD_LIST_HEADER_RE = re.compile(r"^<Thread List \((\d+) entries\)>$")
_THREAD_ROW_RE = re.compile(
    r"^UID: (0x[0-9a-fA-F]{8}) - Name: (.+?)\s*$"
)
_MODULE_LIST_HEADER_RE = re.compile(r"^<Module List \((\d+) modules\)>$")
_MODULE_ROW_RE = re.compile(
    r"^UID: (0x[0-9a-fA-F]{8}) Attr: [0-9a-fA-F]+ - Name: (.+?)\s*$"
)
_MEMINFO_ROW_RE = re.compile(
    r"^\s*(\d+)\s*\|\s*0x[0-9a-fA-F]+\s*\|\s*\d+\s*\|"
    r"\s*(\d+)\s*\|\s*(\d+)\s*\|\s*[0-9a-fA-F]+\s*\|\s*$"
)
_MODULE_THREAD_HEADER_RE = re.compile(r"^Module Thread \((\d+)\)$")
_PROBE_COMPLETE_RE = re.compile(
    r"^NAKAGAWA_PSP_COMPLETE schema=1 status=(PASS|FAIL|NOT_RUN)$", re.MULTILINE
)


def parse_psplink_thread_snapshot(text: str) -> frozenset[tuple[str, str]]:
    """Parse PSPLink's `thlist`, refusing truncated or unrecognized output."""

    lines = [line.strip() for line in text.splitlines()]
    headers = [match for line in lines if (match := _THREAD_LIST_HEADER_RE.fullmatch(line))]
    if len(headers) != 1:
        raise ValueError("thlist must contain exactly one Thread List header")
    rows = [match for line in lines if (match := _THREAD_ROW_RE.fullmatch(line))]
    threads = {(match.group(1).lower(), match.group(2)) for match in rows}
    if len(threads) != len(rows) or len(rows) != int(headers[0].group(1)):
        raise ValueError("thlist count does not match its unique UID/name rows")
    return frozenset(threads)


def parse_psplink_meminfo(text: str) -> dict[int, tuple[int, int]]:
    """Return partition -> (total free bytes, largest free block bytes)."""

    upper = text.upper()
    if "MEMORY PARTITIONS:" not in upper or "TOTALFREE" not in upper or "MAXFREE" not in upper:
        raise ValueError("meminfo is missing its partition/free-memory header")
    rows = [match for line in text.splitlines() if (match := _MEMINFO_ROW_RE.fullmatch(line))]
    partitions = {
        int(match.group(1)): (int(match.group(2)), int(match.group(3)))
        for match in rows
    }
    if not rows or len(partitions) != len(rows):
        raise ValueError("meminfo has no partition rows or contains duplicate partitions")
    return partitions


def parse_psplink_module_list(text: str) -> frozenset[tuple[str, str]]:
    """Parse PSPLink's all-module `modlist` output as UID/name pairs."""

    lines = [line.strip() for line in text.splitlines()]
    headers = [match for line in lines if (match := _MODULE_LIST_HEADER_RE.fullmatch(line))]
    if len(headers) != 1:
        raise ValueError("modlist must contain exactly one Module List header")
    rows = [match for line in lines if (match := _MODULE_ROW_RE.fullmatch(line))]
    modules = {(match.group(1).lower(), match.group(2)) for match in rows}
    if len(modules) != len(rows) or len(rows) != int(headers[0].group(1)):
        raise ValueError("modlist count does not match its unique UID/name rows")
    return frozenset(modules)


def parse_psplink_module_threads(text: str, module_uid: str) -> frozenset[tuple[str, str]]:
    """Return threads PSPLink reports as belonging to one module."""

    uid = module_uid.lower()
    if not re.fullmatch(r"0x[0-9a-f]{8}", uid):
        raise ValueError("module UID must be an eight-digit hexadecimal PSPLink UID")
    lines = [line.strip() for line in text.splitlines()]
    module_rows = [match for line in lines if (match := _MODULE_ROW_RE.fullmatch(line))]
    if not any(match.group(1).lower() == uid for match in module_rows):
        raise ValueError("module-specific modinfo output does not name the requested UID")
    headers = [
        (index, match)
        for index, line in enumerate(lines)
        if (match := _MODULE_THREAD_HEADER_RE.fullmatch(line))
    ]
    if len(headers) != 1:
        raise ValueError("module modinfo must contain exactly one Module Thread section")
    start, header = headers[0]
    rows = [
        match for line in lines[start + 1:]
        if (match := _THREAD_ROW_RE.fullmatch(line))
    ]
    threads = {(match.group(1).lower(), match.group(2)) for match in rows}
    if len(rows) != int(header.group(1)) or len(threads) != len(rows):
        raise ValueError("module thread count does not match its unique UID/name rows")
    return frozenset(threads)


def parse_probe_completion_sentinel(text: str) -> str | None:
    """Return one final PASS/FAIL/NOT_RUN marker, preserving its actual state."""
    matches = list(_PROBE_COMPLETE_RE.finditer(text))
    if len(matches) != 1 or not text.endswith("\n"):
        return None
    last_line = next((line for line in reversed(text.splitlines()) if line.strip()), "")
    return matches[0].group(1) if matches[0].group(0) == last_line else None


def _has_probe_completion_sentinel(text: str) -> bool:
    """Recognize one final marker through the canonical parser."""
    return parse_probe_completion_sentinel(text) is not None


def evaluate_teardown_snapshots(
    before: PsplinkSnapshot,
    after_probe: PsplinkSnapshot | None,
    after_unload: PsplinkSnapshot | None,
    module_uid: str,
    module_threads: set[tuple[str, str]] | frozenset[tuple[str, str]] | None,
    *,
    unload_confirmed: bool | None,
    sentinel_status: str | None,
    shell_qualified: bool | None,
    host0_roundtrip: bool | None,
    case_succeeded: bool = True,
    host0_capture_complete: bool = True,
    module_threads_available: bool = True,
    exprint_command_status: str = "NOT_RUN",
) -> dict[str, object]:
    """Compare owned teardown state and retain allocator deltas as diagnostics.

    Global PSP free-memory counters can move independently of this probe. The
    probe's tracked-resource cleanup and PASS sentinel, plus thread/module
    inventories and the unload handshake, are the teardown contract; meminfo
    values are preserved here for observation but do not gate that contract.
    """

    uid = module_uid.lower()
    s0_memory = dict(before.memory_free_bytes)
    s1_memory = dict(after_probe.memory_free_bytes) if after_probe is not None else {}
    s2_memory = dict(after_unload.memory_free_bytes) if after_unload is not None else {}
    memory_deltas: dict[int, dict[str, dict[str, int] | None]] = {}
    for partition in sorted(set(s0_memory) | set(s1_memory) | set(s2_memory)):
        baseline = s0_memory.get(partition)
        memory_deltas[partition] = {}
        for stage, readings in (("s1", s1_memory), ("s2", s2_memory)):
            current = readings.get(partition)
            memory_deltas[partition][stage] = (
                None
                if baseline is None or current is None
                else {
                    "total_free": current[0] - baseline[0],
                    "largest_block": current[1] - baseline[1],
                }
            )
    thread_delta = (
        set(after_probe.threads) - set(before.threads)
        if after_probe is not None else set()
    )
    disappeared_before_unload = (
        set(before.threads) - set(after_probe.threads)
        if after_probe is not None else set()
    )
    module_uid_in_s1 = (
        any(item[0] == uid for item in after_probe.modules)
        if after_probe is not None else False
    )
    module_uid_in_s2 = (
        any(item[0] == uid for item in after_unload.modules)
        if after_unload is not None else False
    )
    issues: list[str] = []
    blocked = False
    confirmed_teardown_failure = False
    if after_probe is None:
        issues.append("S1 snapshot unavailable")
        blocked = True
    else:
        if not module_uid_in_s1:
            issues.append("probe module is absent from S1 before unload")
            confirmed_teardown_failure = True
        if not module_threads_available or module_threads is None:
            issues.append("module thread query unavailable")
            blocked = True
        else:
            if len(module_threads) != 1:
                issues.append("module must own exactly one remaining main thread at S1")
                confirmed_teardown_failure = True
            if thread_delta != set(module_threads):
                issues.append("S1 contains leaked child thread(s) or an unexpected thread delta")
                confirmed_teardown_failure = True
        if disappeared_before_unload:
            issues.append("S1 is missing a pre-probe thread")
            confirmed_teardown_failure = True
    leftover_threads: set[tuple[str, str]] = set()
    missing_threads: set[tuple[str, str]] = set()
    if after_unload is None:
        issues.append("S2 snapshot unavailable")
        blocked = True
    else:
        leftover_threads = set(after_unload.threads) - set(before.threads)
        missing_threads = set(before.threads) - set(after_unload.threads)
        if after_probe is not None and set(after_unload.threads) != set(before.threads):
            issues.append("post-unload thread set differs from S0")
            confirmed_teardown_failure = True
            if leftover_threads and module_threads and leftover_threads <= set(module_threads):
                # Name the boundary: the module was stopped and unloaded but the
                # thread it created survived, so its module_stop did not end it.
                issues.append(
                    "probe main thread survived module stop/unload; "
                    "the probe's module_stop did not end and delete it"
                )
        if set(after_unload.modules) != set(before.modules):
            issues.append("post-unload module set differs from S0")
            confirmed_teardown_failure = True
        if module_uid_in_s2:
            issues.append("probe module UID remains loaded after unload")
            confirmed_teardown_failure = True
    if unload_confirmed is None:
        issues.append("modstun unload handshake unavailable")
        blocked = True
    elif not unload_confirmed:
        issues.append("modstun stop/unload handshake or Unknown module check failed")
        confirmed_teardown_failure = True
    if sentinel_status is None:
        issues.append("probe completion sentinel unavailable")
        blocked = True
    elif sentinel_status == "NOT_RUN":
        issues.append("probe completion sentinel reports NOT_RUN")
        blocked = True
    elif sentinel_status == "FAIL":
        issues.append("probe completion sentinel reports FAIL")
        confirmed_teardown_failure = True
    elif sentinel_status != "PASS":
        issues.append("probe completion sentinel is malformed")
        blocked = True
    if not host0_capture_complete:
        issues.append("host0 result capture is incomplete or unavailable")
        blocked = True
    if shell_qualified is None:
        issues.append("PSPLink shell qualification unavailable after unload")
        blocked = True
    elif not shell_qualified:
        issues.append("PSPLink shell is not qualified after unload")
    if host0_roundtrip is None:
        issues.append("host0 round-trip result unavailable")
        blocked = True
    elif not host0_roundtrip:
        issues.append("host0 round-trip failed after unload")

    status = "BLOCKED" if blocked else "FAIL" if issues else "PASS"
    recovery_eligible = (
        status == "FAIL"
        and case_succeeded
        and confirmed_teardown_failure
        and shell_qualified is True
        and host0_roundtrip is True
        and sentinel_status in {"PASS", "FAIL"}
    )
    return {
        "status": status,
        "issues": issues,
        "recovery_eligible": recovery_eligible,
        "recovery_status": "NOT_RUN",
        "probe_succeeded": case_succeeded,
        "shell_qualified": shell_qualified,
        "s0_thread_count": len(before.threads),
        "s1_thread_count": len(after_probe.threads) if after_probe is not None else None,
        "s2_thread_count": len(after_unload.threads) if after_unload is not None else None,
        "s2_leftover_threads": [list(item) for item in sorted(leftover_threads)],
        "s2_missing_threads": [list(item) for item in sorted(missing_threads)],
        "s0_module_count": len(before.modules),
        "s2_module_count": len(after_unload.modules) if after_unload is not None else None,
        "module_uid": uid,
        "memory_free_deltas_bytes": memory_deltas,
        "memory_free_deltas_diagnostic_only": True,
        "exprint_status": "NOT_RUN",
        "exprint_command_status": exprint_command_status,
    }


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
        "model": args.model,
        "model_code": args.model_code,
        "results_directory": str(args.results_directory.relative_to(ROOT)).replace("\\", "/"),
        "provenance_supplied": all(getattr(args, flag) for flag in PROVENANCE_FLAGS),
        "manual_steps_remaining": [
            "connect a human-configured PSPLink session",
            "confirm host0 round-trip before launching a probe",
            "supply --binary/--source-commit/--model/--firmware so the capture is acceptance-eligible",
        ],
    }


def _run_command(command: list[str], timeout: float) -> tuple[int | None, str, str, str]:
    def output_text(value: str | bytes | None) -> str:
        if isinstance(value, bytes):
            return value.decode("utf-8", errors="replace")
        return value or ""

    try:
        completed = subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
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
        return (
            None,
            output_text(exc.stdout)[-65536:],
            output_text(exc.stderr)[-65536:],
            "TIMEOUT",
        )
    except OSError as exc:
        return None, "", type(exc).__name__, "ERROR"
    status = "PROCESS_EXITED"
    return completed.returncode, completed.stdout[-65536:], completed.stderr[-65536:], status


def _verify_psplink_shell(
    run: Callable[[str, float], tuple[int | None, str, str, str]],
    deadline: float,
    *,
    take_unknown_command_events: Callable[[], int] | None = None,
    record_event: Callable[[str], None] | None = None,
    clock: Callable[[], float] = time.monotonic,
) -> tuple[bool, tuple[int | None, str, str, str] | None, int, str]:
    """Verify only the PSPLink shell, retrying bounded transport losses."""

    if not math.isfinite(deadline) or deadline <= 0:
        return False, None, 0, "shell verification deadline must be finite and positive"

    end = clock() + deadline
    last_result = None
    attempts = 0
    for attempt in range(1, SHELL_VERIFICATION_ATTEMPTS + 1):
        remaining = end - clock()
        if remaining <= 0:
            break
        timeout = min(remaining, SHELL_VERIFICATION_ATTEMPT_TIMEOUT)
        attempts = attempt
        result = run("ver", timeout)
        last_result = result
        unknown_events = (
            take_unknown_command_events() if take_unknown_command_events else 0
        )
        output = result[1] + "\n" + result[2]
        unknown_command = unknown_events > 0 or "error, unknown command" in output.casefold()
        verified = (
            result[0] == 0
            and result[3] == "PROCESS_EXITED"
            and "psplink" in output.casefold()
            and not unknown_command
        )
        if verified:
            if record_event:
                record_event(
                    f"shell verification attempt {attempt}/{SHELL_VERIFICATION_ATTEMPTS}: PASS"
                )
            return True, result, attempts, f"PSPLink shell verified by `ver` in {attempts} attempt(s)"

        if unknown_command:
            failure = "retryable transport event: USBHostFS reported `Error, unknown command`"
        elif result[3] == "TIMEOUT":
            failure = "retryable transport event: `ver` reply timed out or was lost"
        else:
            failure = (
                "retryable transport event: `ver` did not identify PSPLink "
                f"(status={result[3]}, rc={result[0]})"
            )
        if record_event:
            record_event(
                f"shell verification attempt {attempt}/{SHELL_VERIFICATION_ATTEMPTS}: {failure}"
            )

    detail = (
        f"PSPLink shell verification exhausted {attempts} of "
        f"{SHELL_VERIFICATION_ATTEMPTS} `ver` attempt(s) within {deadline:g}s; "
        "manual commands: `usbipd list`, `usbipd attach --wsl --busid <BUSID>`, "
        "`pspsh -e ver`"
    )
    return False, last_result, attempts, detail


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


@dataclass(frozen=True)
class CampaignCase:
    case_id: str
    binary: Path
    timeout: float


@dataclass
class _CaseProgress:
    """What one case has done so far, so a host-side error can still clean up."""

    before: PsplinkSnapshot | None = None
    launched: bool = False
    module_uid: str | None = None
    unload_attempted: bool = False
    unload_status: str | None = None
    recorded: bool = False


def _host_error_summary(exc: BaseException) -> str:
    """Name a host-side exception by type, raising function and message.

    The location comes from the innermost traceback frame. An OSError keeps
    only its strerror so that no local path enters the evidence.
    """

    frames = traceback.extract_tb(exc.__traceback__)
    where = (
        f" in {frames[-1].name} ({Path(frames[-1].filename).name}:{frames[-1].lineno})"
        if frames else ""
    )
    if isinstance(exc, OSError):
        message = exc.strerror or ""
    else:
        message = str(exc).splitlines()[0] if str(exc) else ""
    message = message[:240]
    return f"{type(exc).__name__}{where}" + (f": {message}" if message else "")


def _wsl_path(path: Path) -> str:
    """Convert a Windows path to the standard WSL /mnt/<drive> form."""

    drive, tail = os.path.splitdrive(str(path))
    if not drive:
        return path.as_posix()
    return f"/mnt/{drive[0].lower()}/{tail.lstrip('\\/').replace('\\', '/')}"


def _check_source_tree(source_commit: str) -> str | None:
    """Require the declared source revision to be the clean repository HEAD."""

    try:
        head = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
            shell=False,
        )
        status = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=normal"],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
            shell=False,
        )
    except OSError as exc:
        return f"source checkout could not be verified ({type(exc).__name__})"
    if head.returncode != 0 or status.returncode != 0:
        return "source checkout could not be verified by Git"
    current_head = head.stdout.strip()
    if status.stdout.strip():
        return "source worktree is dirty"
    if current_head.casefold() != source_commit.casefold():
        return "declared source commit does not match the clean checkout HEAD"
    return None


def _host0_remote_path(binary: Path, host0_root: object) -> str:
    """The host0: path of a staged PRX, keeping its subdirectory under the root.

    A transport without a host0 root (the simulated test transports) has no
    staging layout to honour, so the bare name is used there. A PRX outside a
    real root is a staging error: refuse it here rather than issue an ldstart
    the device cannot resolve.
    """

    if not isinstance(host0_root, Path):
        return binary.name
    try:
        return binary.resolve().relative_to(host0_root.resolve()).as_posix()
    except ValueError as exc:
        raise ValueError(f"campaign PRX {binary} is not inside host0 root {host0_root}") from exc


def _campaign_host0_log_path(host0_root: Path, case_id: str) -> Path:
    """Return the source-owned probe log path for one campaign case."""

    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", case_id):
        raise ValueError("campaign case id must be a simple path component")
    if re.fullmatch(r"dmac-size-matrix-size-0x[0-9a-f]{8}", case_id):
        return host0_root / "dmac_size_matrix_cell_log.txt"
    stem = case_id.replace("-", "_")
    if stem.startswith("dma_"):
        stem = "dmac_" + stem[4:]
    return host0_root / f"{stem}_log.txt"


def _parse_fixed_campaign_records(text: str, case_id: str):
    """Validate the exact record order and scalar fields of legacy fixed probes."""

    try:
        test_id, contract = _FIXED_CAMPAIGN_CASES[case_id]
    except KeyError as exc:
        raise ProtocolError(f"{case_id}: no fixed campaign contract") from exc
    parsed = parse_output(text)
    if any(record.test_id != test_id for record in parsed.results):
        raise ProtocolError(f"{case_id}: stream contains a foreign test_id")
    expected_cases = tuple(item[0] for item in contract)
    observed_cases = tuple(record.case_id for record in parsed.results)
    if observed_cases != expected_cases:
        raise ProtocolError(
            f"{case_id}: expected ordered cases {expected_cases}, got {observed_cases}"
        )
    for record, (expected_case, out_count, statuses) in zip(
        parsed.results, contract, strict=True
    ):
        if record.case_id != expected_case or record.status not in statuses:
            raise ProtocolError(
                f"{case_id}: {record.case_id} has an unexpected status or position"
            )
        values = dict(record.values)
        expected_fields = {"result", *(f"out{index}" for index in range(out_count))}
        if set(values) != expected_fields:
            raise ProtocolError(
                f"{case_id}: {record.case_id} fields must be "
                f"{sorted(expected_fields)}, got {sorted(values)}"
            )
        record_check = _FIXED_CAMPAIGN_RECORD_CHECKS.get(case_id)
        if record_check is not None:
            record_check(record)
    return parsed


def _validate_campaign_contract(text: str, case_id: str) -> None:
    """Raise ProtocolError unless ``text`` meets the case's completion contract.

    The case validators return differently shaped reports (a SequenceReport
    keys its results by case_id; the registry census wraps its records), so
    their return values are deliberately not used as the typed record view.
    """

    if case_id in DMAC_INVALID_CASES:
        parse_dmac_invalid_tail_output(text, case_id)
    elif case_id in {"dma-size-matrix", "dmac-size-matrix"}:
        validate_dmac_size_matrix(text)
    elif match := re.fullmatch(r"dmac-size-matrix-size-0x([0-9a-f]{8})", case_id):
        validate_dmac_size_matrix_size(text, int(match.group(1), 16))
    elif case_id == "hle-ge-edram":
        # Registered as a fixed-shape stream, plus the restore invariant: a
        # stream that leaves the GE translation width changed is a protocol
        # failure, not a measurement.
        parse_campaign_probe_output(text, case_id, require_complete=True)
        validate_hle_edram_restore(text)
    elif case_id in CAMPAIGN_PROBE_CASES:
        parse_campaign_probe_output(text, case_id, require_complete=True)
    elif case_id == "registry-readonly":
        parse_registry_readonly_output(text, require_complete=True)
    elif case_id in _FIXED_CAMPAIGN_CASES:
        _parse_fixed_campaign_records(text, case_id)
    elif case_id in _COMPLETE_CAMPAIGN_PARSERS:
        _COMPLETE_CAMPAIGN_PARSERS[case_id](text, require_complete=True)
    elif case_id in _SINGLE_RECORD_CAMPAIGN_CASES:
        expected = _SINGLE_RECORD_CAMPAIGN_CASES[case_id]
        parsed = parse_output(text)
        if len(parsed.results) != 1:
            raise ProtocolError(f"{case_id} stream must contain exactly one result record")
        if (parsed.results[0].test_id, parsed.results[0].case_id) != expected:
            raise ProtocolError(f"{case_id} stream must contain {expected[0]}/{expected[1]}")


_COMPLETE_CAMPAIGN_PARSERS = {
    "audio-query": parse_audio_query_output,
    "fpu-vector": parse_fpu_vector_output,
    "cache-alias": parse_cache_alias_output,
    "io-matrix": parse_io_matrix_output,
    "mbx-delete-wait": parse_mbx_delete_wait_output,
    "ge-nan": parse_ge_nan_output,
    "dma-cells": parse_dmac_cells_output,
    "delay-zero": parse_delay_zero_output,
}
_SINGLE_RECORD_CAMPAIGN_CASES = {
    "transport-write": ("PSP-TRANSPORT-001", "host0-write-readback"),
    "model-profile": ("PSP-SYSTEM-001", "model-profile"),
}


def _parse_campaign_records(text: str, case_id: str) -> ParsedOutput:
    """Validate a campaign's known completion contract, then return its typed rows.

    Every caller iterates ``results`` as TestResult records (status, values,
    metadata). This is the single typed view for all campaign cases, whatever
    shape the case-specific validator reports.
    """

    _validate_campaign_contract(text, case_id)
    return parse_output(text)


def _campaign_stream_complete(text: str, case_id: str) -> bool:
    """Return whether a fresh campaign stream satisfies its known completion contract."""

    if _campaign_completeness_contract(case_id) == "unregistered-no-completion-contract":
        return False
    completion_markers = list(_PROBE_COMPLETE_RE.finditer(text))
    if completion_markers:
        # The final probe marker is a teardown control record, not a campaign
        # result row. Validate its shape here; the caller separately records
        # whether it says PASS, FAIL, or NOT_RUN in the teardown verdict.
        if not _has_probe_completion_sentinel(text):
            return False
        text = text[:completion_markers[0].start()]
    try:
        parsed = _parse_campaign_records(_normalise_unbound_identity_fields(text), case_id)
    except (ProtocolError, OSError, UnicodeError, ValueError):
        return False
    return bool(parsed.results)


def _normalise_unbound_identity_fields(text: str) -> str:
    """Make absent or abbreviated device identity parseable without binding it.

    The raw values remain available to the envelope's identity comparison. This
    schema-only view replaces unusable identity fields with valid placeholders,
    allowing the strict result-completion gate to validate the actual records.
    """

    lines = text.splitlines()
    metadata_indices = [
        index for index, line in enumerate(lines)
        if line.startswith("NAKAGAWA_PSP_META ")
    ]
    if len(metadata_indices) != 1:
        return text

    index = metadata_indices[0]
    tokens = lines[index].split()
    fields: list[str] = []
    seen: set[str] = set()
    for token in tokens[1:]:
        key, separator, value = token.partition("=")
        if key not in {"source_commit", "binary_sha256"}:
            fields.append(token)
            continue
        if not separator or key in seen:
            return text
        seen.add(key)
        if key == "source_commit":
            value = (
                value.lower()
                if _FULL_COMMIT_RE.fullmatch(value) and not _ALL_ZERO_RE.fullmatch(value)
                else "0" * 40
            )
        else:
            value = (
                value.lower()
                if _FULL_SHA256_RE.fullmatch(value) and not _ALL_ZERO_RE.fullmatch(value)
                else "0" * 64
            )
        fields.append(f"{key}={value}")

    if "source_commit" not in seen:
        fields.append(f"source_commit={'0' * 40}")
    if "binary_sha256" not in seen:
        fields.append(f"binary_sha256={'0' * 64}")
    lines[index] = tokens[0] + " " + " ".join(fields)
    return "\n".join(lines) + ("\n" if text.endswith("\n") else "")


def _campaign_completeness_contract(case_id: str) -> str:
    if case_id in CAMPAIGN_PROBE_CASES or case_id == "registry-readonly":
        return "strict-golden-sequence"
    if case_id in _FIXED_CAMPAIGN_CASES:
        return "strict-fixed-record-sequence"
    if case_id in {"dma-size-matrix", "dmac-size-matrix"} or re.fullmatch(
        r"dmac-size-matrix-size-0x[0-9a-f]{8}", case_id
    ):
        return "strict-dmac-sequence"
    if case_id in {"audio-query", "fpu-vector", "cache-alias", "io-matrix",
                   "mbx-delete-wait", "ge-nan", "dma-cells", "delay-zero"}:
        return "strict-golden-sequence"
    if case_id in DMAC_INVALID_CASES:
        return "strict-dmac-invalid-cell-records"
    if case_id in {"transport-write", "model-profile"}:
        return "exactly-one-known-record"
    return "unregistered-no-completion-contract"


def _model_profile_raw_value(parsed) -> str | None:
    """Return the exact raw model scalar reported by the model-profile probe."""

    profiles = [
        item for item in parsed.results
        if item.test_id == "PSP-SYSTEM-001" and item.case_id == "model-profile"
    ]
    if not profiles:
        return None
    if len(profiles) != 1:
        raise ProtocolError("model-profile stream contains duplicate model observations")
    values = dict(profiles[0].values)
    raw_value = values.get("out0")
    result_value = values.get("result")
    if raw_value is None or result_value is None:
        raise ProtocolError("model-profile stream is missing its raw model result")
    if int(raw_value, 0) != int(result_value, 0):
        raise ProtocolError("model-profile result does not match its raw out0 value")
    return raw_value


def _validate_model_code_expectation(raw_value: str | None, expected: int | None) -> None:
    if raw_value is not None and expected is not None and int(raw_value, 0) != expected:
        raise ProtocolError(
            f"device model-profile out0={raw_value} does not match "
            f"the supplied raw model value {expected}"
        )


def _render_argv(template: list[str], **values: str) -> list[str]:
    rendered = list(template)
    for key, value in values.items():
        marker = "{" + key + "}"
        rendered = [part.replace(marker, value) for part in rendered]
    return rendered


def _parse_usbipd_psplink_devices(output: str) -> list[tuple[str, str]]:
    """Return (busid, state) rows for PSPLink devices in ``usbipd list`` output."""

    devices = []
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 3 or fields[1].casefold() != "054c:01c9":
            continue
        if len(fields) >= 4 and fields[-2].casefold() == "not":
            state = "Not shared" if fields[-1].casefold() == "shared" else fields[-1]
        else:
            state = fields[-1]
        devices.append((fields[0], state))
    return devices


DEFAULT_TRANSPORT_START_TIMEOUT = 60.0
_SERVER_OUTPUT_TAIL_LINES = 8


class TransportStartError(RuntimeError):
    """The host USB link to PSPLink did not become ready.

    Start-up runs no PSPLink shell command, so nothing reached the PSP and a
    failure here never needs a power cycle.
    """


class PsplinkProcessTransport:
    """Real host process adapter for one standalone PSPLINK/USBHostFS route.

    ``start`` returns only after a positive readiness signal: USBHostFS is
    running its device poll loop, the PSPLink device is attached to the USB/IP
    client (attaching it once when ``usbipd`` reports it ``Shared``), and
    USBHostFS has printed ``Connected to device``. That line means USBHostFS
    opened the device inside the client, which is stronger evidence than the
    device merely being listed there. Every wait shares one bounded budget.

    USBHostFS prints ``waiting for device...`` once each time it enters its
    poll loop: at start-up and after every disconnect. Only a wait that follows
    a connection is a device loss; the start-up wait is not.
    """

    def __init__(
        self,
        *,
        pspsh_argv: list[str],
        usbhostfs_argv: list[str],
        host0_root: Path,
        session_id: str,
        usbipd_argv: list[str] | None = None,
        command_runner: Callable[[list[str], float], tuple[int | None, str, str, str]] = _run_command,
        popen_factory: Callable[..., subprocess.Popen] = subprocess.Popen,
        start_timeout: float = DEFAULT_TRANSPORT_START_TIMEOUT,
    ) -> None:
        if not math.isfinite(start_timeout) or start_timeout <= 0:
            raise ValueError("transport start timeout must be finite and positive")
        self.session_id = session_id
        self.pspsh_argv = list(pspsh_argv)
        self.usbhostfs_argv = list(usbhostfs_argv)
        self.usbipd_argv = list(usbipd_argv or ["usbipd"])
        self.host0_root = host0_root.resolve()
        self.command_runner = command_runner
        self.popen_factory = popen_factory
        self.start_timeout = start_timeout
        self.start_detail: str | None = None
        self.server: subprocess.Popen | None = None
        self._server_output_thread: threading.Thread | None = None
        self._server_state = threading.Condition()
        self._server_polling = False
        self._link_up = False
        self._device_lost = False
        self._server_output_closed = False
        self._server_output_tail: list[str] = []
        self._server_generation = 0
        self._unknown_command_events = 0

    def _server_argv(self) -> list[str]:
        return _render_argv(
            self.usbhostfs_argv,
            host0_root=str(self.host0_root),
            host0_root_wsl=_wsl_path(self.host0_root),
        )

    def _reset_server_state(self) -> int:
        """Forget the previous server; return the new server's output generation."""

        with self._server_state:
            self._server_generation += 1
            self._server_polling = False
            self._link_up = False
            self._device_lost = False
            self._server_output_closed = False
            self._server_output_tail = []
            self._unknown_command_events = 0
            return self._server_generation

    def check_hardware_lock(self) -> None:
        """Raise HardwareLockError unless this session holds the hardware lock."""

        require_hardware_lock(self.session_id)

    def start(self) -> None:
        """Start USBHostFS and return only once the PSPLink USB link is up."""

        self.check_hardware_lock()
        if not self.host0_root.is_dir():
            raise FileNotFoundError("host0 root must be an existing directory")
        deadline = time.monotonic() + self.start_timeout
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) if os.name == "nt" else 0
        self.start_detail = None
        generation = self._reset_server_state()
        self.server = self.popen_factory(
            self._server_argv(),
            cwd=self.host0_root,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
            shell=False,
            creationflags=flags,
        )
        if self.server.stdout is not None:
            self._server_output_thread = threading.Thread(
                target=self._read_server_output,
                args=(self.server.stdout, generation),
                name="usbhostfs-output",
                daemon=True,
            )
            self._server_output_thread.start()
        if self.server.poll() is not None:
            raise TransportStartError(
                self._with_server_output("usbhostfs_pc exited during startup")
            )
        self.start_detail = self._establish_link_at_start(deadline)

    def _establish_link_at_start(self, deadline: float) -> str:
        if not self._wait_server(lambda: self._server_polling, deadline):
            raise TransportStartError(self._start_wait_failure(
                "usbhostfs_pc did not report `waiting for device` or `Connected to device`"
            ))
        if self._link_is_up():
            return "USBHostFS reported `Connected to device`"
        attached, detail, busid, action = self._attach_psplink_device(
            max(deadline - time.monotonic(), 0.0)
        )
        if not attached:
            raise TransportStartError(detail)
        if not self._wait_server(lambda: self._link_up, deadline):
            raise TransportStartError(self._start_wait_failure(
                f"USBHostFS did not report `Connected to device` for PSPLink device {busid} "
                f"({action})",
                "check that PSPLink is running on the PSP, then run `usbipd list` and "
                "`wsl lsusb -d 054c:01c9`",
            ))
        return f"PSPLink device {busid} {action}; USBHostFS reported `Connected to device`"

    def _start_wait_failure(self, detail: str, guidance: str = "") -> str:
        with self._server_state:
            closed = self._server_output_closed
        if closed:
            detail += " before usbhostfs_pc exited or closed its output"
        else:
            detail += f" within the {self.start_timeout:g}s transport start budget"
        if guidance:
            detail += f"; {guidance}"
        return self._with_server_output(detail)

    def _with_server_output(self, detail: str) -> str:
        with self._server_state:
            tail = list(self._server_output_tail)
        if not tail:
            return detail + "; usbhostfs_pc printed nothing"
        return detail + "; last usbhostfs_pc output: " + " | ".join(tail)

    def _wait_server(self, predicate: Callable[[], bool], deadline: float) -> bool:
        """Wait for a USBHostFS state, failing early if its output has ended."""

        with self._server_state:
            while True:
                if predicate():
                    return True
                if self.server is None or self._server_output_closed:
                    return False
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                self._server_state.wait(remaining)

    def _link_is_up(self) -> bool:
        with self._server_state:
            return self._link_up

    def _observe_server_output(self, line: str, generation: int | None = None) -> None:
        normalized = line.casefold()
        with self._server_state:
            if generation is not None and generation != self._server_generation:
                return  # a previous server's late output never describes this one
            text = line.strip()
            if text:
                self._server_output_tail.append(text[:200])
                del self._server_output_tail[:-_SERVER_OUTPUT_TAIL_LINES]
            if "waiting for device" in normalized:
                self._server_polling = True
                if self._link_up:
                    self._device_lost = True
                self._link_up = False
            if "connected to device" in normalized:
                self._server_polling = True
                self._link_up = True
            if "error, unknown command" in normalized:
                self._unknown_command_events += 1
            self._server_state.notify_all()

    def _read_server_output(self, stream, generation: int) -> None:
        try:
            for line in stream:
                self._observe_server_output(line, generation)
        except (OSError, ValueError):
            pass
        finally:
            with self._server_state:
                if generation == self._server_generation:
                    self._server_output_closed = True
                    self._server_state.notify_all()

    def take_waiting_for_device(self) -> bool:
        """Return and clear one device loss: a USBHostFS wait after a connection."""

        with self._server_state:
            lost, self._device_lost = self._device_lost, False
        return lost

    def take_unknown_command_events(self) -> int:
        with self._server_state:
            events, self._unknown_command_events = self._unknown_command_events, 0
        return events

    @staticmethod
    def _command_succeeded(result: tuple[int | None, str, str, str]) -> bool:
        return result[0] == 0 and result[3] == "PROCESS_EXITED"

    def _attach_psplink_device(self, timeout: float) -> tuple[bool, str, str | None, str]:
        """Find the one PSPLink device in ``usbipd list``; attach it once when Shared.

        Returns ``(attached, detail, busid, action)``. The adapter never binds or
        detaches a device: those stay manual and are named in ``detail``.
        """

        list_result = self.command_runner(self.usbipd_argv + ["list"], timeout)
        if not self._command_succeeded(list_result):
            return (
                False,
                "usbipd list could not report the PSPLink device; manual command: `usbipd list`",
                None,
                "",
            )
        devices = _parse_usbipd_psplink_devices(list_result[1] + "\n" + list_result[2])
        if not devices:
            return (
                False,
                "PSPLink device 054c:01c9 is absent from usbipd list; reconnect the PSP, "
                "then run `usbipd list` and `usbipd attach --wsl --busid <BUSID>`",
                None,
                "",
            )
        if len(devices) != 1:
            return (
                False,
                "multiple PSPLink devices 054c:01c9 are present in usbipd list; "
                "disconnect extras and run `usbipd list` to identify the PSP busid",
                None,
                "",
            )

        busid, state = devices[0]
        if state.casefold() == "not shared":
            return (
                False,
                f"PSPLink device {busid} is not bound (usbipd state: Not shared); "
                f"manual commands: `usbipd bind --busid {busid}` then "
                f"`usbipd attach --wsl --busid {busid}`",
                busid,
                "",
            )
        if state.casefold() not in {"shared", "attached"}:
            return (
                False,
                f"PSPLink device {busid} has unsupported usbipd state `{state}`; "
                f"manual command: `usbipd list`",
                busid,
                "",
            )
        if state.casefold() == "attached":
            return True, f"PSPLink device {busid} already attached", busid, "already attached"

        # A detached device cannot still be connected; forget any connection
        # USBHostFS has not yet reported losing so that only a fresh
        # `Connected to device` can satisfy the wait that follows.
        with self._server_state:
            self._link_up = False
        attach_result = self.command_runner(
            self.usbipd_argv + ["attach", "--wsl", "--busid", busid], timeout
        )
        if not self._command_succeeded(attach_result):
            return (
                False,
                f"usbipd attach failed for PSPLink device {busid}; "
                f"manual command: `usbipd attach --wsl --busid {busid}`",
                busid,
                "",
            )
        return True, f"PSPLink device {busid} attached from Shared", busid, "attached from Shared"

    def recover_psplink_transport(
        self,
        timeout: float,
        *,
        shell_verification_timeout: float = DEFAULT_SHELL_VERIFICATION_TIMEOUT,
        record_event: Callable[[str], None] | None = None,
    ) -> tuple[bool, str, tuple[int | None, str, str, str] | None]:
        """Reattach once, then qualify the fresh shell with bounded ``ver`` attempts."""

        self.take_waiting_for_device()
        attached, detail, busid, action = self._attach_psplink_device(timeout)
        if not attached:
            return False, detail, None
        if not self._wait_server(lambda: self._link_up, time.monotonic() + timeout):
            verb = "attaching" if action == "attached from Shared" else "re-attaching"
            return (
                False,
                f"USBHostFS did not report `Connected to device` after {verb} PSPLink "
                f"device {busid}; manual command: `usbipd attach --wsl --busid {busid}`",
                None,
            )

        verified, version, attempts, verification_detail = _verify_psplink_shell(
            self.run,
            shell_verification_timeout,
            take_unknown_command_events=self.take_unknown_command_events,
            record_event=record_event,
        )
        if not verified:
            return (
                False,
                f"PSPLink transport re-attach for device {busid} did not qualify: "
                f"{verification_detail}",
                version,
            )
        return (
            True,
            f"PSPLink device {busid} {action}; {verification_detail}",
            version,
        )

    def run(self, remote_command: str, timeout: float) -> tuple[int | None, str, str, str]:
        marker = "{remote_command}"
        if any(marker in part for part in self.pspsh_argv):
            command = _render_argv(self.pspsh_argv, remote_command=remote_command)
        else:
            command = self.pspsh_argv + ["-e", remote_command]
        return self.command_runner(command, timeout)

    def restart(self) -> None:
        self.stop()
        self.start()

    def stop(self) -> None:
        server, self.server = self.server, None
        if server is None:
            return
        if server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=2.0)
        if self._server_output_thread is not None:
            self._server_output_thread.join(timeout=0.2)
            self._server_output_thread = None
        if server.stdout is not None:
            server.stdout.close()
        with self._server_state:
            self._server_polling = False
            self._link_up = False
            self._server_state.notify_all()


class PsplinkCampaignRunner:
    """Bounded multi-probe adapter; uses the same L0-L4 recovery contract."""

    _MODULE_UID_RE = re.compile(r"\bUID:\s*(0x[0-9a-fA-F]+)")
    _FIRMWARE_RE = re.compile(r"\bVersion:\s*([0-9]+\.[0-9]+(?:\.[0-9]+)?)\b")
    _RECORD_PREFIXES = ("NAKAGAWA_PSP_META ", "NAKAGAWA_PSP_TEST ")

    def __init__(
        self,
        transport: PsplinkProcessTransport,
        *,
        console_model: str,
        source_commit: str,
        model_code: int | None = None,
        expected_firmware: str | None = None,
        qualification_timeout: float = 5.0,
        cleanup_timeout: float = 5.0,
        shell_verification_timeout: float = DEFAULT_SHELL_VERIFICATION_TIMEOUT,
    ) -> None:
        if not math.isfinite(shell_verification_timeout) or shell_verification_timeout <= 0:
            raise ValueError("shell verification timeout must be finite and positive")
        firmware_problem = _expected_firmware_problem(expected_firmware)
        if firmware_problem:
            raise ValueError(firmware_problem)
        self.transport = transport
        self.console_model = console_model
        self.source_commit = source_commit
        self.model_code = model_code
        self.expected_firmware = expected_firmware
        self.qualification_timeout = qualification_timeout
        self.cleanup_timeout = cleanup_timeout
        self.shell_verification_timeout = shell_verification_timeout
        self.state = "OFFLINE"
        self.terminal_reason: str | None = None
        self.recovery_events: list[str] = []
        self.envelopes: list[dict[str, object]] = []
        self.firmware: str | None = None
        self.host0_qualified = False
        self.source_tree_problem: str | None = None
        self.intervention_case_id: str | None = None
        self.resume_case_index: int | None = None
        self.transport_start_problem: str | None = None
        self.hardware_lock_status: str | None = None
        self.last_modstun_reply: str | None = None
        self.host_error: str | None = None
        # False only when a host-side error ended a launched case whose teardown
        # was then verified clean: the case is lost but the PSP needs no power cycle.
        self.intervention_requires_power_cycle = True
        self._l0_cleanup_attempted = False
        self._l1_attempted = False
        self._l1_active = False
        self._l1_transport_reattach_attempted = False
        self._l2_reset_attempted = False
        self._l2_transport_reattach_attempted = False

    @staticmethod
    def _ok(result: tuple[int | None, str, str, str]) -> bool:
        return result[0] == 0 and result[3] == "PROCESS_EXITED"

    def _request(self, command: str, timeout: float | None = None):
        actual_timeout = self.qualification_timeout if timeout is None else timeout
        if self.terminal_reason is not None:
            return None, "", "", "ERROR"
        result = self.transport.run(command, actual_timeout)
        take_waiting = getattr(self.transport, "take_waiting_for_device", None)
        if command != "reset" and callable(take_waiting) and take_waiting():
            recovered, detail, verification = self._recover_usb_transport(
                "USBHostFS reported `waiting for device`", actual_timeout
            )
            if recovered:
                if command == "ver" and verification is not None:
                    return verification
                if command in {"usbstat", "pwd", "pspver"}:
                    return self.transport.run(command, actual_timeout)
                self.recovery_events.append(
                    f"L1: discarded `{command}` result because transport reported a device wait"
                )
                self.state = "STOPPED"
                self.terminal_reason = "TRANSPORT_RESULT_DISCARDED"
                return None, "", "", "TRANSPORT_RECOVERED"
            return None, "", "", "ERROR"
        return result

    def _recover_usb_transport(
        self, trigger: str, timeout: float, *, recovery_level: str = "L1"
    ) -> tuple[bool, str, tuple[int | None, str, str, str] | None]:
        if recovery_level == "L1":
            if self._l1_transport_reattach_attempted:
                detail = "L1 PSPLink transport re-attach limit exhausted"
                self.recovery_events.append(f"L1: {detail} after {trigger}")
                return False, detail, None
            if not self._l1_active:
                if self._l1_attempted:
                    detail = "L1 recovery limit exhausted"
                    return False, detail, None
                self._l1_attempted = True
            self._l1_transport_reattach_attempted = True
        elif recovery_level == "L2":
            if self._l2_transport_reattach_attempted:
                detail = "L2 PSPLink transport re-attach limit exhausted"
                self._physical_intervention(f"{trigger}: {detail}")
                return False, detail, None
            self._l2_transport_reattach_attempted = True
        else:
            raise ValueError(f"unsupported recovery level {recovery_level!r}")
        recover = getattr(self.transport, "recover_psplink_transport", None)
        if not callable(recover):
            detail = "transport adapter does not implement PSPLink re-attach"
            if recovery_level == "L2":
                self._physical_intervention(f"{trigger}: {detail}")
            return False, detail, None
        self.recovery_events.append(
            f"{recovery_level}: re-attach PSPLink transport after {trigger}"
        )
        try:
            recovered, detail, verification = recover(
                timeout,
                shell_verification_timeout=self.shell_verification_timeout,
                record_event=self.recovery_events.append,
            )
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            recovered = False
            detail = f"PSPLink transport re-attach failed ({type(exc).__name__})"
            verification = None
        if not recovered:
            if recovery_level == "L2":
                self._physical_intervention(f"{trigger}: {detail}")
            else:
                self.recovery_events.append(
                    f"L1: {detail}; continue to L2 reset"
                )
            return False, detail, verification
        self.recovery_events.append(f"{recovery_level}: {detail}")
        return True, detail, verification

    def _shell_qualified(self) -> bool:
        verified, _version, _attempts, _detail = _verify_psplink_shell(
            self.transport.run,
            self.shell_verification_timeout,
            take_unknown_command_events=getattr(
                self.transport, "take_unknown_command_events", None
            ),
            record_event=self.recovery_events.append,
        )
        if not verified:
            return False
        usbstat = self._request("usbstat")
        pwd = self._request("pwd")
        return (
            self._ok(usbstat) and "established" in usbstat[1].lower()
            and self._ok(pwd) and "host0:/" in pwd[1]
        )

    def _qualify(self) -> bool:
        if not self._shell_qualified():
            return False
        pspver = self._request("pspver")
        if not self._ok(pspver):
            return False
        match = self._FIRMWARE_RE.search(pspver[1])
        if not match:
            return False
        self.firmware = match.group(1)
        if self.expected_firmware and self.firmware != self.expected_firmware:
            self.terminal_reason = "IDENTITY_MISMATCH"
            self.state = "STOPPED"
            return False
        self.state = "READY"
        return True

    def _take_snapshot(self) -> tuple[PsplinkSnapshot | None, str | None]:
        parsed: dict[str, object] = {}
        parsers = (
            ("thlist", parse_psplink_thread_snapshot),
            ("meminfo", parse_psplink_meminfo),
            ("modlist", parse_psplink_module_list),
        )
        for command, parser in parsers:
            result = self._request(command, self.cleanup_timeout)
            if not self._ok(result):
                return None, f"{command} command failed"
            try:
                parsed[command] = parser(result[1] + result[2])
            except ValueError as exc:
                return None, f"{command} output could not be parsed ({exc})"
        memory = parsed["meminfo"]
        if not isinstance(memory, dict):
            return None, "meminfo parser returned an invalid snapshot"
        return PsplinkSnapshot(
            threads=parsed["thlist"],
            memory_free_bytes=tuple(sorted(memory.items())),
            modules=parsed["modlist"],
        ), None

    def _module_threads(
        self, module_uid: str
    ) -> tuple[frozenset[tuple[str, str]], str | None]:
        result = self._request(f"modinfo {module_uid} t", self.cleanup_timeout)
        if not self._ok(result):
            return frozenset(), "module thread query failed"
        try:
            return parse_psplink_module_threads(result[1] + result[2], module_uid), None
        except ValueError as exc:
            return frozenset(), f"module thread output could not be parsed ({exc})"

    @staticmethod
    def _probe_case_succeeded(
        case: CampaignCase,
        result: tuple[int | None, str, str, str],
        module_uid: str | None,
        *,
        run_started_ns: int | None,
        host0_log_cleared: bool,
        captured_host0_text: str | None,
        captured_host0_mtime_ns: int | None,
        host0_capture_problem: str | None,
    ) -> bool:
        """Require fresh, complete, passing case evidence before teardown recovery."""
        if (
            result[0] != 0
            or result[3] != "PROCESS_EXITED"
            or not module_uid
            or run_started_ns is None
            or not host0_log_cleared
            or captured_host0_text is None
            or captured_host0_mtime_ns is None
            or host0_capture_problem is not None
            or parse_probe_completion_sentinel(captured_host0_text) not in {"PASS", "FAIL"}
            or not (
                run_started_ns - HOST0_MTIME_TOLERANCE_NS
                <= captured_host0_mtime_ns
                <= time.time_ns() + HOST0_MTIME_TOLERANCE_NS
            )
            or not _campaign_stream_complete(captured_host0_text, case.case_id)
        ):
            return False
        result_text = captured_host0_text
        completion = _PROBE_COMPLETE_RE.search(result_text)
        if completion is not None:
            result_text = result_text[:completion.start()]
        try:
            parsed = _parse_campaign_records(
                _normalise_unbound_identity_fields(result_text), case.case_id
            )
        except (ProtocolError, OSError, UnicodeError, ValueError):
            return False
        # A probe's explicit SKIP is a completed safety outcome, not evidence
        # of the skipped semantic. The envelope keeps SKIP ineligible as a
        # measured result while teardown may still be checked normally.
        return bool(parsed.results) and all(
            item.status in {"PASS", "SKIP"} for item in parsed.results
        )

    def _enforce_teardown_check(
        self,
        report: dict[str, object],
        module_uid: str | None = None,
        *,
        probe_succeeded: bool = False,
    ) -> bool:
        if report.get("status") == "PASS":
            report["recovery_status"] = "NOT_REQUIRED"
            return True
        if (
            report.get("status") != "FAIL"
            or report.get("recovery_eligible") is not True
            or not probe_succeeded
        ):
            report["recovery_status"] = "NOT_RUN"
            self.state = "STOPPED"
            self.terminal_reason = (
                "TEARDOWN_CHECK_BLOCKED"
                if report.get("status") == "BLOCKED"
                else "TEARDOWN_RECOVERY_NOT_ELIGIBLE"
            )
            return False
        issues = report.get("issues", [])
        detail = "; ".join(str(issue) for issue in issues)
        recovered = self._recover(module_uid, f"probe teardown check failed: {detail}")
        report["recovery_status"] = "RECOVERED" if recovered else "FAILED"
        return recovered

    def _unload_status(self, module_uid: str) -> str:
        stopped = self._request(f"modstun {module_uid}", self.cleanup_timeout)
        stop_text = stopped[1] + stopped[2]
        # Keep PSPLink's own reply ("Module Stop/Unload <stop>/<unload> Status
        # <module_stop return>") as teardown evidence; the probe's module_stop
        # returns 0 only after it ended and deleted main.
        self.last_modstun_reply = next(
            (line.strip()[:200] for line in stop_text.splitlines()
             if line.strip().startswith("Module Stop/Unload")),
            None,
        )
        if stopped[3] != "PROCESS_EXITED":
            return "BLOCKED"
        if stopped[0] != 0 or "Module Stop/Unload 0x00000000/" not in stop_text:
            return "FAIL" if stopped[0] not in {0, None} else "BLOCKED"
        info = self._request(f"modinfo {module_uid}", self.cleanup_timeout)
        text = info[1] + info[2]
        if info[3] != "PROCESS_EXITED":
            return "BLOCKED"
        if info[0] != 0 and "Unknown module" in text:
            return "PASS"
        return "BLOCKED"

    def _unload(self, module_uid: str) -> bool:
        """Compatibility predicate for callers that only need confirmed unload."""
        return self._unload_status(module_uid) == "PASS"

    def _settle_after_unload(self) -> tuple[str, int]:
        """Settle the link after `modstun` before any post-unload check uses it.

        On 2026-10-10 seven single-case hardware runs captured complete data and
        confirmed the unload, then failed the post-unload shell qualification and
        host0 round-trip, the way `pspsh ver` fails right after USBHostFS
        connects. The settle issues `ver` through the shared bounded verifier
        until PSPLink answers, so S2 and the shell qualification run on a link
        that has answered since the unload; the round-trip after them is a
        host-side file check and issues no PSPLink command. The settle never
        decides the teardown verdict: when it is exhausted those checks still
        run and classify the case as they would without it. Its attempts carry
        their own event prefix so they never count as qualification attempts.
        """
        if self.terminal_reason is not None:
            # A stopped session issues no further PSPLink commands (see _request).
            return "NOT_RUN", 0
        verified, _version, attempts, _detail = _verify_psplink_shell(
            self.transport.run,
            self.shell_verification_timeout,
            take_unknown_command_events=getattr(
                self.transport, "take_unknown_command_events", None
            ),
            record_event=lambda event: self.recovery_events.append(
                f"post-unload settle: {event}"
            ),
        )
        return ("PASS" if verified else "EXHAUSTED"), attempts

    def _verify_host0_roundtrip(self) -> bool:
        root = getattr(self.transport, "host0_root", None)
        if not isinstance(root, Path):
            return False
        path = root / "nakagawa_transport_write.bin"
        try:
            captured = path.read_bytes()
        except OSError:
            return False
        expected = bytes(
            (0x5A ^ (index * 0x25 + (index >> 3))) & 0xFF
            for index in range(64)
        )
        try:
            path.unlink()
        except OSError:
            return False
        return captured == expected

    def _physical_intervention(self, detail: str) -> None:
        self.recovery_events.append(f"L4: {detail}")
        self.state = "SESSION_WEDGED"
        self.terminal_reason = "PHYSICAL_INTERVENTION_REQUIRED"

    def _hardware_lock_held(self, stage: str) -> bool:
        """Re-read the hardware lock; stop before touching the PSP when it is not held."""

        try:
            self.transport.check_hardware_lock()
        except HardwareLockError as exc:
            self.recovery_events.append(
                f"HARDWARE_LOCK: {stage}: {exc.status}; stopped before touching the PSP"
            )
            self.hardware_lock_status = exc.status
            self.state = "STOPPED"
            self.terminal_reason = "HARDWARE_LOCK_REFUSED"
            return False
        return True

    def _transport_start_failed(self, detail: str) -> None:
        """Stop before any launch: the PSP ran nothing, so no power cycle is needed."""

        self.recovery_events.append(
            f"TRANSPORT_START: {detail}; no probe was launched, so no power cycle is required"
        )
        self.transport_start_problem = detail
        self.state = "STOPPED"
        self.terminal_reason = "TRANSPORT_START_FAILED"

    def _reset_once(self, detail: str) -> bool:
        if self._l2_reset_attempted:
            self._physical_intervention("L2 reset limit exhausted")
            return False
        try:
            shell_qualified = self._shell_qualified()
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            self.recovery_events.append(
                "L2: shell qualification raised "
                f"{type(exc).__name__}; stop at L4"
            )
            self._physical_intervention(
                "PSPLink shell qualification raised during L2 recovery"
            )
            return False
        if not shell_qualified:
            if self.terminal_reason is None:
                self.recovery_events.append(
                    "L2: reset not attempted because PSPLink shell qualification failed"
                )
                self._physical_intervention("PSPLink did not qualify before L2 reset")
            return False
        if not self._hardware_lock_held("before PSPLink reset"):
            return False
        self._l2_reset_attempted = True
        self.recovery_events.append(f"L2: {detail}")
        try:
            reset = self._request("reset", self.cleanup_timeout)
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            self.recovery_events.append(
                "L2: reset command raised "
                f"{type(exc).__name__}; stop at L4"
            )
            self._physical_intervention(
                "PSPLink reset command raised before transport re-attach"
            )
            return False
        reattached, _, _ = self._recover_usb_transport(
            "PSPLink reset", self.cleanup_timeout, recovery_level="L2"
        )
        if not reattached:
            return False
        if reset[3] not in {"PROCESS_EXITED", "TIMEOUT"}:
            self._physical_intervention(
                "PSPLink reset command could not run after transport re-attach; "
                "manual command: `pspsh -e reset`"
            )
            return False
        if reset[0] not in {0, None}:
            self.recovery_events.append(
                f"L2: reset process exited {reset[0]}; continue only after `ver` and shell qualification"
            )
        try:
            qualified = self._qualify()
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            self.recovery_events.append(
                "L2: post-reset qualification raised "
                f"{type(exc).__name__}; stop at L4"
            )
            self._physical_intervention(
                "PSPLink post-reset qualification raised after transport re-attach"
            )
            return False
        if not qualified:
            if self.terminal_reason == "IDENTITY_MISMATCH":
                return False
            if self.terminal_reason is None:
                self._physical_intervention(
                    "qualified PSPLink reset and one transport re-attach did not restore "
                    "the shell; manual commands: `usbipd list`, `pspsh -e ver`"
                )
            return False
        return True

    def _recover(self, module_uid: str | None, detail: str) -> bool:
        self.state = "RECOVERABLE_FAULT"
        if not self._l0_cleanup_attempted:
            self._l0_cleanup_attempted = True
            self.recovery_events.append(f"L0: one shell qualification/cleanup retry ({detail})")
            try:
                shell_qualified = self._shell_qualified()
            except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
                self.recovery_events.append(
                    "L0: shell qualification raised "
                    f"{type(exc).__name__}; continue to L1"
                )
                shell_qualified = False
            unload_status = None
            if shell_qualified and module_uid:
                try:
                    unload_status = self._unload_status(module_uid)
                except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
                    self.recovery_events.append(
                        "L0: unload verification raised "
                        f"{type(exc).__name__}; continue to L1"
                    )
            if shell_qualified and unload_status == "PASS":
                self.state = "READY"
                return True
        if self.terminal_reason is not None:
            return False

        if not self._l1_attempted:
            self._l1_attempted = True
            self._l1_active = True
            self.recovery_events.append("L1: restart owned usbhostfs_pc process and requalify")
            try:
                restart_succeeded = False
                try:
                    self.transport.restart()
                except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
                    self.recovery_events.append(
                        "L1: host stack restart raised "
                        f"{type(exc).__name__}; continue to L2"
                    )
                else:
                    restart_succeeded = True
                if self.terminal_reason is not None:
                    return False
                if restart_succeeded:
                    try:
                        qualified = self._qualify()
                    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
                        self.recovery_events.append(
                            "L1: post-restart qualification raised "
                            f"{type(exc).__name__}; continue to L2"
                        )
                        qualified = False
                    if self.terminal_reason == "IDENTITY_MISMATCH":
                        return False
                    if qualified and module_uid:
                        try:
                            unload_status = self._unload_status(module_uid)
                        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
                            self.recovery_events.append(
                                "L1: unload verification raised "
                                f"{type(exc).__name__}; continue to L2"
                            )
                        else:
                            if unload_status == "PASS":
                                self.state = "READY"
                                return True
            finally:
                self._l1_active = False
        return self._reset_once("reset after L1 requalification")

    @staticmethod
    def _sha256(path: Path) -> str:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()

    def _envelope(
        self,
        case: CampaignCase,
        result: tuple[int | None, str, str, str],
        module_uid: str | None,
        cleanup_ok: bool,
        *,
        host0_log_path: Path | None,
        run_started_ns: int | None,
        run_finished_ns: int,
        host0_log_cleared: bool,
        captured_host0_text: str | None,
        captured_host0_mtime_ns: int | None,
        host0_capture_problem: str | None,
        teardown_check: dict[str, object] | None = None,
    ) -> dict[str, object]:
        host0_text = captured_host0_text or ""
        host0_mtime_ns = captured_host0_mtime_ns
        host0_fresh = False
        host0_read_problem = host0_capture_problem
        if host0_log_path is None:
            host0_read_problem = host0_read_problem or "host0 root is unavailable for per-case logs"
        elif not host0_log_cleared:
            host0_read_problem = (
                host0_read_problem
                or "previous host0 log could not be cleared before loading the probe"
            )
        elif captured_host0_text is None:
            host0_read_problem = host0_read_problem or (
                "per-case host0 log was not observed complete before probe unload"
            )
        else:
            # Bind the envelope to the stable bytes observed before unload;
            # never re-read a possibly completed post-unload file here.
            host0_fresh = (
                host0_mtime_ns is not None
                and run_started_ns is not None
                and run_started_ns - HOST0_MTIME_TOLERANCE_NS
                <= host0_mtime_ns
                <= run_finished_ns + HOST0_MTIME_TOLERANCE_NS
            )

        def schema_records(text: str) -> str:
            lines = [
                line.strip()
                for line in text.splitlines()
                if line.strip().startswith(self._RECORD_PREFIXES)
            ]
            return "\n".join(lines) + ("\n" if lines else "")

        host0_record_text = schema_records(host0_text)
        stdout_record_text = schema_records(result[1]) if result[3] == "PROCESS_EXITED" else ""
        blockers: list[str] = []
        qualification_blockers: list[str] = []

        def disqualify(message: str) -> None:
            if message not in qualification_blockers:
                qualification_blockers.append(message)
            if message not in blockers:
                blockers.append(message)

        if _campaign_completeness_contract(case.case_id) == "unregistered-no-completion-contract":
            disqualify(
                f"completion contract for {case.case_id} is unregistered "
                "(in the works: issue #352)"
            )
        # A probe that did not finish names where it stopped: its durable STEP
        # markers in the captured host0 stream. Markers are progress, not results,
        # so they are kept even though the partial stream is not a semantic result.
        probe_stopped = result[3] != "PROCESS_EXITED" or host0_capture_problem is not None
        last_step = None
        if probe_stopped:
            try:
                last_step = parse_progress(host0_text).last_step
            except ProtocolError:
                disqualify("probe progress markers failed strict protocol validation")
        if result[3] == "TIMEOUT":
            disqualify(
                "per-case timeout; partial output is not a semantic result"
                + ("" if last_step is None
                   else f"; last step marker: {last_step.case_id}/{last_step.step}")
            )
        elif result[0] != 0:
            disqualify("PSPLink command did not exit successfully")
        if host0_read_problem:
            disqualify(host0_read_problem)
        if host0_text and not host0_fresh:
            disqualify("host0 log modification time is outside this case's run window")
        if self.source_tree_problem:
            disqualify(self.source_tree_problem)
        if not module_uid or not cleanup_ok:
            disqualify("loaded module was not proven unloaded")
        if not self.host0_qualified:
            disqualify("host0 round-trip qualification did not pass")
        if self.terminal_reason:
            disqualify(self.terminal_reason)

        binary_sha = ""
        try:
            binary_sha = self._sha256(case.binary)
        except OSError as exc:
            disqualify(f"campaign PRX could not be hashed ({type(exc).__name__})")

        host0_parsed = None
        stdout_parsed = None
        device_model_raw_value: str | None = None
        identity: dict[str, object] = {
            "DEVICE_REPORTED_SOURCE_COMMIT": None,
            "DEVICE_REPORTED_BINARY_SHA256": None,
            "SOURCE_COMMIT_BINDING": _IDENTITY_NOT_REPORTED,
            "BINARY_SHA256_BINDING": _IDENTITY_NOT_REPORTED,
            "DEVICE_IDENTITY_STATUS": _IDENTITY_NOT_REPORTED,
            "DEVICE_IDENTITY_BLOCKERS": [],
        }
        canonical = host0_record_text
        parsed_ok = False
        if host0_text:
            metadata_args = argparse.Namespace(
                binary=case.binary,
                model=self.console_model,
                model_code=self.model_code,
                firmware=self.firmware,
                source_commit=self.source_commit,
            )
            identity = _device_identity(host0_text, metadata_args)
            blockers.extend(identity["DEVICE_IDENTITY_BLOCKERS"])
            try:
                host0_parsed = _parse_campaign_records(
                    _normalise_unbound_identity_fields(host0_record_text), case.case_id
                )
                device_model_raw_value = _model_profile_raw_value(host0_parsed)
                _validate_model_code_expectation(device_model_raw_value, self.model_code)
                canonical = _canonicalize_psp(host0_record_text, metadata_args)
                canonical_parsed = _parse_campaign_records(canonical, case.case_id)
                parsed_ok = bool(canonical_parsed.results) and all(
                    item.status == "PASS" for item in canonical_parsed.results
                )
                if not parsed_ok:
                    blockers.append("one or more scalar result records did not pass")
                metadata_problems = provenance_issues(canonical_parsed.metadata_dict())
                if metadata_problems:
                    blockers.append(
                        "captured metadata is not acceptance-eligible: "
                        + "; ".join(metadata_problems)
                    )
            except ProtocolError as exc:
                disqualify(
                    "host0 schema records failed strict protocol validation: "
                    + str(exc)
                )
                canonical = host0_record_text
            except (OSError, UnicodeError, ValueError):
                disqualify("host0 schema records failed strict protocol validation")
                canonical = host0_record_text
        else:
            disqualify("no complete source-owned scalar record was captured from host0")

        if stdout_record_text:
            try:
                stdout_parsed = _parse_campaign_records(
                    _normalise_unbound_identity_fields(stdout_record_text), case.case_id
                )
            except ProtocolError:
                disqualify("stdout schema records failed strict protocol validation")
            else:
                if host0_parsed is not None and (
                    stdout_parsed.metadata != host0_parsed.metadata
                    or stdout_parsed.results != host0_parsed.results
                    or not _device_metadata_equal(
                        _device_metadata(stdout_record_text), _device_metadata(host0_text)
                    )
                ):
                    disqualify("stdout and host0 schema records disagree")

        if case.case_id == "transport-write" and not any(
            "test_id=PSP-TRANSPORT-001" in line
            and "case_id=host0-write-readback" in line
            and "status=PASS" in line
            for line in host0_record_text.splitlines()
        ):
            parsed_ok = False
            blockers.append("transport-write probe did not report a passing host0 round-trip")
        acceptance_eligible = parsed_ok and not blockers
        case_qualified = not qualification_blockers
        model_raw_value: str | int | None = (
            device_model_raw_value if device_model_raw_value is not None else self.model_code
        )
        return {
            **model_identity_fields(self.console_model, model_raw_value),
            "MODEL_SOURCE": "operator-recorded label; no serial or MAC stored",
            "FW": self.firmware or "NOT_CAPTURED",
            "TRANSPORT_PROFILE": "standalone-psplink-usbhostfs-host0",
            "SOURCE_COMMIT": self.source_commit,
            "DEVICE_REPORTED_SOURCE_COMMIT": identity["DEVICE_REPORTED_SOURCE_COMMIT"],
            "DEVICE_REPORTED_BINARY_SHA256": identity["DEVICE_REPORTED_BINARY_SHA256"],
            "SOURCE_COMMIT_BINDING": identity["SOURCE_COMMIT_BINDING"],
            "BINARY_SHA256_BINDING": identity["BINARY_SHA256_BINDING"],
            "DEVICE_IDENTITY_STATUS": identity["DEVICE_IDENTITY_STATUS"],
            "DEVICE_IDENTITY_BLOCKERS": list(identity["DEVICE_IDENTITY_BLOCKERS"]),
            "SOURCE_TREE_STATUS": "CLEAN_COMMITTED" if not self.source_tree_problem else "UNQUALIFIED",
            "SOURCE_TREE_PROBLEM": self.source_tree_problem,
            "BINARY_SHA256": binary_sha,
            "CASE_ID": case.case_id,
            "RAW_RESULT": canonical,
            "RAW_STDOUT_RESULT": stdout_record_text,
            "HOST0_LOG_FILE": host0_log_path.name if host0_log_path else None,
            "HOST0_LOG_CLEARED": host0_log_cleared,
            "HOST0_LOG_MTIME_NS": host0_mtime_ns,
            "HOST0_RUN_STARTED_NS": run_started_ns,
            "HOST0_RUN_FINISHED_NS": run_finished_ns,
            "HOST0_LOG_FRESH": host0_fresh,
            "STREAM_COMPLETENESS_CONTRACT": _campaign_completeness_contract(case.case_id),
            "RECOVERY_EVENTS": list(self.recovery_events),
            "QUALIFICATION_STATUS": "QUALIFIED" if case_qualified else "UNQUALIFIED",
            "QUALIFICATION_BLOCKERS": qualification_blockers,
            # The envelope is built while the case is still RUN_CASE (READY is restored only
            # after it is appended); a recovery that succeeded restores READY. Any other state
            # means the session was lost during this case.
            "SESSION_QUALIFICATION_STATUS": (
                "QUALIFIED"
                if self.state in ("RUN_CASE", "READY")
                or (
                    self.state == "STOPPED"
                    and teardown_check is not None
                    and teardown_check.get("shell_qualified") is True
                    and self.terminal_reason not in {
                        "IDENTITY_MISMATCH", "PHYSICAL_INTERVENTION_REQUIRED"
                    }
                )
                else "LOST"
            ),
            "EVIDENCE_CLASS": "PSP_HARDWARE" if acceptance_eligible else "UNQUALIFIED_CAPTURE",
            "ACCEPTANCE_ELIGIBLE": acceptance_eligible,
            "ACCEPTANCE_BLOCKERS": blockers,
            "TEARDOWN_CHECK": teardown_check,
            "PROCESS_STATUS": result[3],
            "RETURN_CODE": result[0],
            "LAST_PROBE_STEP": (
                None if last_step is None
                else {"case_id": last_step.case_id, "step": last_step.step}
            ),
        }

    def run(
        self,
        cases: list[CampaignCase],
        *,
        require_transport_preflight: bool = True,
        reset_between_cases: bool = False,
        stop_on_incomplete: bool = False,
        case_index_offset: int = 0,
        on_case_start: Callable[[int, CampaignCase, str], None] | None = None,
        on_case_complete: Callable[[int, dict[str, object]], None] | None = None,
    ) -> dict[str, object]:
        if not cases or (
            require_transport_preflight and cases[0].case_id != "transport-write"
        ):
            self.state = "STOPPED"
            self.terminal_reason = "HOST0_ROUNDTRIP_REQUIRED"
            return self._report()
        if not require_transport_preflight and not self.host0_qualified:
            self.state = "STOPPED"
            self.terminal_reason = "HOST0_ROUNDTRIP_REQUIRED"
            return self._report()
        if any(not math.isfinite(case.timeout) or case.timeout <= 0 for case in cases):
            self.state = "STOPPED"
            self.terminal_reason = "INVALID_CASE_TIMEOUT"
            return self._report()
        self.source_tree_problem = _check_source_tree(self.source_commit)
        host0_path = getattr(self.transport, "host0_root", None)
        if isinstance(host0_path, Path) and (
            host0_path / "nakagawa_transport_write.bin"
        ).exists():
            self.state = "STOPPED"
            self.terminal_reason = "HOST0_ROUNDTRIP_PATH_EXISTS"
            return self._report()
        try:
            self.transport.start()
        except HardwareLockError as exc:
            self.transport.stop()
            self.recovery_events.append(
                f"HARDWARE_LOCK: before transport start: {exc.status}; "
                "stopped before touching the PSP"
            )
            self.hardware_lock_status = exc.status
            self.state = "STOPPED"
            self.terminal_reason = "HARDWARE_LOCK_REFUSED"
            return self._report()
        except (OSError, RuntimeError) as exc:
            self.transport.stop()
            self._transport_start_failed(
                str(exc) if isinstance(exc, TransportStartError)
                else f"host transport process could not start ({type(exc).__name__})"
            )
            return self._report()
        start_detail = getattr(self.transport, "start_detail", None)
        if isinstance(start_detail, str) and start_detail:
            self.recovery_events.append(f"transport start: {start_detail}")
        try:
            try:
                qualified = self._qualify()
            except Exception as exc:  # noqa: BLE001 - a host bug must never crash the runner
                self.host_error = f"before the first case: {_host_error_summary(exc)}"
                self.recovery_events.append(
                    f"HOST_ERROR: {self.host_error}; no probe was launched"
                )
                self.state = "STOPPED"
                self.terminal_reason = "HOST_ERROR"
                return self._report()
            if not qualified:
                if self.terminal_reason is None:
                    self._transport_start_failed(
                        "PSPLink shell did not qualify after the USB link came up; check "
                        "that the PSPLink shell is running on the PSP; manual commands: "
                        "`usbipd list`, `pspsh -e ver`"
                    )
                return self._report()
            for local_index, case in enumerate(cases):
                case_index = case_index_offset + local_index
                progress = _CaseProgress()
                try:
                    keep_going = self._run_case(
                        case,
                        case_index,
                        local_index,
                        cases,
                        host0_path,
                        progress,
                        reset_between_cases=reset_between_cases,
                        stop_on_incomplete=stop_on_incomplete,
                        on_case_start=on_case_start,
                        on_case_complete=on_case_complete,
                    )
                except Exception as exc:  # noqa: BLE001 - a host bug must never crash the queue
                    self._contain_case_error(
                        case, case_index, progress, exc, on_case_complete=on_case_complete
                    )
                    break
                if not keep_going:
                    break
        finally:
            self.transport.stop()
        return self._report()

    def _run_case(
        self,
        case: CampaignCase,
        case_index: int,
        local_index: int,
        cases: list[CampaignCase],
        host0_path: object,
        progress: _CaseProgress,
        *,
        reset_between_cases: bool,
        stop_on_incomplete: bool,
        on_case_start: Callable[[int, CampaignCase, str], None] | None,
        on_case_complete: Callable[[int, dict[str, object]], None] | None,
    ) -> bool:
        """Run one queued case; return False when the queue must stop after it."""

        if not self._hardware_lock_held(f"before case {case.case_id}"):
            return False
        if reset_between_cases and local_index > 0:
            if on_case_start is not None:
                try:
                    on_case_start(case_index, case, "RESET_BEFORE_CASE")
                except (OSError, ValueError, TypeError):
                    self.state = "STOPPED"
                    self.terminal_reason = "CHECKPOINT_WRITE_FAILED"
                    return False
            self._l2_reset_attempted = False
            self._l2_transport_reattach_attempted = False
            if not self._reset_once(
                f"campaign soft reset before {case.case_id}"
            ):
                if self.terminal_reason == "PHYSICAL_INTERVENTION_REQUIRED":
                    self.intervention_case_id = cases[local_index - 1].case_id
                    self.resume_case_index = case_index
                return False
        case_host0_log = (
            _campaign_host0_log_path(host0_path, case.case_id)
            if isinstance(host0_path, Path) else None
        )
        host0_log_cleared = False
        if case_host0_log is not None:
            try:
                case_host0_log.unlink(missing_ok=True)
                host0_log_cleared = not (
                    case_host0_log.exists() or case_host0_log.is_symlink()
                )
            except OSError:
                host0_log_cleared = False
        if not host0_log_cleared:
            self.state = "STOPPED"
            self.terminal_reason = (
                "HOST0_LOG_UNAVAILABLE" if case_host0_log is None
                else "HOST0_LOG_CLEAR_FAILED"
            )
            self.envelopes.append(
                self._envelope(
                    case,
                    (None, "", "", self.terminal_reason),
                    None,
                    False,
                    host0_log_path=case_host0_log,
                    run_started_ns=None,
                    run_finished_ns=time.time_ns(),
                    host0_log_cleared=False,
                    captured_host0_text=None,
                    captured_host0_mtime_ns=None,
                    host0_capture_problem="per-case host0 log was unavailable before launch",
                )
            )
            return False

        try:
            remote_path = _host0_remote_path(
                case.binary, getattr(self.transport, "host0_root", None)
            )
        except ValueError:
            # Same structured refusal as the sibling pre-launch failures: a PRX
            # the device cannot resolve under host0 is never launched.
            self.state = "STOPPED"
            self.terminal_reason = "HOST0_PRX_OUTSIDE_ROOT"
            self.envelopes.append(
                self._envelope(
                    case,
                    (None, "", "", self.terminal_reason),
                    None,
                    False,
                    host0_log_path=case_host0_log,
                    run_started_ns=None,
                    run_finished_ns=time.time_ns(),
                    host0_log_cleared=host0_log_cleared,
                    captured_host0_text=None,
                    captured_host0_mtime_ns=None,
                    # No local paths in evidence: name the case, not the files.
                    host0_capture_problem=(f"campaign PRX for case {case.case_id} "
                                           "is not inside host0 root"),
                )
            )
            return False
        before, snapshot_problem = self._take_snapshot()
        progress.before = before
        if before is None:
            self.state = "STOPPED"
            self.terminal_reason = "TEARDOWN_S0_SNAPSHOT_FAILED"
            self.envelopes.append(
                self._envelope(
                    case,
                    (None, "", "", self.terminal_reason),
                    None,
                    False,
                    host0_log_path=case_host0_log,
                    run_started_ns=None,
                    run_finished_ns=time.time_ns(),
                    host0_log_cleared=host0_log_cleared,
                    captured_host0_text=None,
                    captured_host0_mtime_ns=None,
                    host0_capture_problem=snapshot_problem,
                    teardown_check={
                        "status": "BLOCKED",
                        "stage": "S0",
                        "issues": [snapshot_problem or "baseline snapshot failed"],
                    },
                )
            )
            return False
        if not self._hardware_lock_held(f"before launching {case.case_id}"):
            return False
        run_started_ns = time.time_ns()
        if on_case_start is not None:
            try:
                on_case_start(case_index, case, "CASE_ACTIVE")
            except (OSError, ValueError, TypeError):
                self.state = "STOPPED"
                self.terminal_reason = "CHECKPOINT_WRITE_FAILED"
                return False
        self.state = "RUN_CASE"
        progress.launched = True
        result = self._request(f"ldstart host0:/{remote_path}", case.timeout)
        uid_match = self._MODULE_UID_RE.search(result[1])
        module_uid = uid_match.group(1) if uid_match else None
        progress.module_uid = module_uid

        captured_host0_text: str | None = None
        captured_host0_mtime_ns: int | None = None
        host0_capture_problem: str | None = None
        if case_host0_log is None:
            host0_capture_problem = "host0 root is unavailable for per-case logs"
        elif not host0_log_cleared:
            host0_capture_problem = (
                "previous host0 log could not be cleared before loading the probe"
            )
        elif result[3] == "PROCESS_EXITED":
            try:
                # The probe appends its completion marker after every record,
                # so the marker ends the stream. The record contract is checked
                # afterwards: a final stream that violates it is a named
                # protocol failure, not a reason to keep waiting.
                capture = _wait_for_host0_output(
                    case_host0_log,
                    case.timeout,
                    not_before_ns=run_started_ns - HOST0_MTIME_TOLERANCE_NS,
                    ready=_has_probe_completion_sentinel,
                    include_mtime=True,
                )
                if not isinstance(capture, tuple):
                    raise RuntimeError("host0 wait did not return captured metadata")
                captured_host0_text, captured_host0_mtime_ns = capture
            except TimeoutError:
                partial, partial_mtime_ns, partial_problem = _snapshot_host0_output(
                    case_host0_log
                )
                host0_capture_problem = (
                    "per-case host0 stream reached no completion marker within the "
                    f"{case.timeout:g}s case timeout ("
                    + _host0_progress_detail(partial, partial_mtime_ns, run_started_ns)
                    + ")"
                )
                captured_host0_text = partial
                captured_host0_mtime_ns = partial_mtime_ns
                if partial_problem:
                    host0_capture_problem += f"; {partial_problem}"
            except UnsafeHost0OutputError as exc:
                host0_capture_problem = str(exc)
            except (OSError, UnicodeError) as exc:
                host0_capture_problem = (
                    "per-case host0 log could not be observed before probe unload "
                    f"({type(exc).__name__})"
                )
        else:
            host0_capture_problem = (
                "per-case command timed out before a complete host0 stream was observed"
            )
            captured_host0_text, captured_host0_mtime_ns, partial_problem = (
                _snapshot_host0_output(case_host0_log)
            )
            if partial_problem:
                host0_capture_problem += f"; {partial_problem}"

        # Stop only when the probe may not have finished. A finished probe whose
        # records violate their contract is torn down normally; its envelope
        # names the protocol failure.
        if stop_on_incomplete and (
            result[0] != 0
            or result[3] != "PROCESS_EXITED"
            or captured_host0_text is None
            or parse_probe_completion_sentinel(captured_host0_text) is None
        ):
            self._physical_intervention(
                f"campaign stopped after incomplete or uncertain case {case.case_id}; "
                "maintainer power-cycle confirmation is required before continuing"
            )
            self.intervention_case_id = case.case_id
            self.resume_case_index = case_index + 1
            run_finished_ns = time.time_ns()
            teardown_report = {
                "status": "BLOCKED",
                "stage": "PROBE",
                "issues": [host0_capture_problem or "complete case stream was not observed"],
                "recovery_status": "NOT_RUN",
            }
            envelope = self._envelope(
                case,
                result,
                module_uid,
                False,
                host0_log_path=case_host0_log,
                run_started_ns=run_started_ns,
                run_finished_ns=run_finished_ns,
                host0_log_cleared=host0_log_cleared,
                captured_host0_text=captured_host0_text,
                captured_host0_mtime_ns=captured_host0_mtime_ns,
                host0_capture_problem=host0_capture_problem,
                teardown_check=teardown_report,
            )
            self.envelopes.append(envelope)
            progress.recorded = True
            if on_case_complete is not None:
                try:
                    on_case_complete(case_index, envelope)
                except (OSError, ValueError, TypeError):
                    self.terminal_reason = "CHECKPOINT_WRITE_FAILED"
            return False

        after_probe, s1_problem = self._take_snapshot()
        module_threads: frozenset[tuple[str, str]] = frozenset()
        module_thread_problem: str | None = None
        if module_uid:
            module_threads, module_thread_problem = self._module_threads(module_uid)
        else:
            module_thread_problem = "ldstart did not return a module UID"

        self.last_modstun_reply = None
        progress.unload_attempted = True
        unload_status = (
            self._unload_status(module_uid) if module_uid else "BLOCKED"
        )
        progress.unload_status = unload_status
        # Only an issued stop/unload handshake disturbs the link; without a UID
        # no `modstun` ran and there is nothing to settle.
        settle_status, settle_attempts = (
            self._settle_after_unload() if module_uid else ("NOT_RUN", 0)
        )
        after_unload, s2_problem = self._take_snapshot()
        shell_qualified = self._shell_qualified()
        exprint = self._request("exprint", self.cleanup_timeout)
        host0_roundtrip_ok = (
            self._verify_host0_roundtrip()
            if case.case_id == "transport-write" or not reset_between_cases
            else self.host0_qualified
        )
        probe_succeeded = self._probe_case_succeeded(
            case,
            result,
            module_uid,
            run_started_ns=run_started_ns,
            host0_log_cleared=host0_log_cleared,
            captured_host0_text=captured_host0_text,
            captured_host0_mtime_ns=captured_host0_mtime_ns,
            host0_capture_problem=host0_capture_problem,
        )
        teardown_report = evaluate_teardown_snapshots(
            before,
            after_probe,
            after_unload,
            module_uid or "0x00000000",
            module_threads,
            unload_confirmed=(
                True if unload_status == "PASS"
                else False if unload_status == "FAIL"
                else None
            ),
            sentinel_status=parse_probe_completion_sentinel(
                captured_host0_text or ""
            ),
            shell_qualified=shell_qualified,
            host0_roundtrip=host0_roundtrip_ok,
            case_succeeded=probe_succeeded,
            host0_capture_complete=(
                captured_host0_text is not None
                and host0_capture_problem is None
            ),
            module_threads_available=module_thread_problem is None,
            exprint_command_status=exprint[3],
        )
        teardown_report["modstun_reply"] = self.last_modstun_reply
        teardown_report["settle_status"] = settle_status
        teardown_report["settle_attempts"] = settle_attempts
        for stage, problem in (("S1", s1_problem), ("S2", s2_problem)):
            if problem:
                teardown_report["issues"].append(
                    f"{stage} snapshot capture failed: {problem}"
                )
        if host0_capture_problem:
            teardown_report["issues"].append(host0_capture_problem)
        if module_thread_problem:
            teardown_report["issues"].append(
                f"module thread snapshot unavailable: {module_thread_problem}"
            )
        cleanup_ok = unload_status == "PASS" and teardown_report["status"] == "PASS"
        if teardown_report["status"] != "PASS":
            if stop_on_incomplete:
                self._physical_intervention(
                    f"campaign stopped after teardown check failed for {case.case_id}; "
                    "maintainer power-cycle confirmation is required before continuing"
                )
                self.intervention_case_id = case.case_id
                self.resume_case_index = case_index + 1
                teardown_report["recovery_status"] = "NOT_RUN"
            else:
                self._enforce_teardown_check(
                    teardown_report,
                    module_uid,
                    probe_succeeded=probe_succeeded,
                )
        run_finished_ns = time.time_ns()
        if case.case_id == "transport-write":
            self.host0_qualified = bool(host0_roundtrip_ok)
        if (
            (case.case_id == "transport-write" or not reset_between_cases)
            and
            not host0_roundtrip_ok
            and self.terminal_reason in {
                None, "TEARDOWN_CHECK_BLOCKED",
                "TEARDOWN_RECOVERY_NOT_ELIGIBLE",
            }
        ):
            self.state = "STOPPED"
            self.terminal_reason = "HOST0_ROUNDTRIP_FAILED"
        envelope = self._envelope(
                case,
                result,
                module_uid,
                cleanup_ok,
                host0_log_path=case_host0_log,
                run_started_ns=run_started_ns,
                run_finished_ns=run_finished_ns,
                host0_log_cleared=host0_log_cleared,
                captured_host0_text=captured_host0_text,
                captured_host0_mtime_ns=captured_host0_mtime_ns,
                host0_capture_problem=host0_capture_problem,
                teardown_check=teardown_report,
            )
        self.envelopes.append(envelope)
        progress.recorded = True
        if on_case_complete is not None:
            try:
                on_case_complete(case_index, envelope)
            except (OSError, ValueError, TypeError):
                self.state = "STOPPED"
                self.terminal_reason = "CHECKPOINT_WRITE_FAILED"
        if self.terminal_reason:
            return False
        self.state = "READY"
        return True

    def _contain_case_error(
        self,
        case: CampaignCase,
        case_index: int,
        progress: _CaseProgress,
        exc: Exception,
        *,
        on_case_complete: Callable[[int, dict[str, object]], None] | None,
    ) -> None:
        """End a case that raised on the host: name the error, tear down, checkpoint.

        Nothing launched: stop with HOST_ERROR and keep the queue position.
        A launched case that was not yet recorded: unload the probe if it was
        not unloaded, take S2, and compare it with S0. A verified-clean
        teardown records the case as interrupted without a power cycle; any
        other outcome demands one, exactly like an incomplete case.
        """

        detail = _host_error_summary(exc)
        self.host_error = f"{case.case_id}: {detail}"
        self.recovery_events.append(f"HOST_ERROR: {self.host_error}")
        if not progress.launched or progress.recorded:
            self.state = "STOPPED"
            self.terminal_reason = "HOST_ERROR"
            return

        # Tear down before naming the stop: a terminal reason blocks PSPLink requests.
        teardown: dict[str, object] = {
            "status": "BLOCKED",
            "stage": "HOST_ERROR",
            "issues": [f"host-side error while processing the case: {detail}"],
            "recovery_status": "NOT_RUN",
        }
        clean = False
        try:
            unload_status = progress.unload_status
            if progress.module_uid and not progress.unload_attempted:
                self.last_modstun_reply = None
                unload_status = self._unload_status(progress.module_uid)
                teardown["modstun_reply"] = self.last_modstun_reply
            teardown["unload_status"] = unload_status or "BLOCKED"
            after_unload, snapshot_problem = self._take_snapshot()
            if snapshot_problem:
                teardown["issues"].append(f"S2 snapshot capture failed: {snapshot_problem}")
            if after_unload is not None and progress.before is not None:
                leftover = set(after_unload.threads) - set(progress.before.threads)
                missing = set(progress.before.threads) - set(after_unload.threads)
                teardown["s2_leftover_threads"] = [list(item) for item in sorted(leftover)]
                teardown["s2_missing_threads"] = [list(item) for item in sorted(missing)]
                clean = (
                    unload_status == "PASS"
                    and not leftover and not missing
                    and set(after_unload.modules) == set(progress.before.modules)
                )
        except Exception as teardown_exc:  # noqa: BLE001 - containment must not raise
            teardown["issues"].append(
                f"host-side teardown attempt failed: {_host_error_summary(teardown_exc)}"
            )
            clean = False
        teardown["post_error_teardown_clean"] = clean
        self.intervention_case_id = case.case_id
        self.resume_case_index = case_index + 1
        self.state = "STOPPED"
        self.terminal_reason = "HOST_ERROR"
        if clean:
            self.intervention_requires_power_cycle = False
            self.recovery_events.append(
                f"HOST_ERROR: {case.case_id} teardown verified clean after the error; "
                "the case is recorded as interrupted and needs no power cycle"
            )
        else:
            self._physical_intervention(
                f"host-side error during {case.case_id} and its teardown was not verified "
                "clean; maintainer power-cycle confirmation is required before continuing"
            )
        envelope: dict[str, object] = {
            "CASE_ID": case.case_id,
            "SOURCE_COMMIT": self.source_commit,
            "FW": self.firmware or "NOT_CAPTURED",
            "PROCESS_STATUS": "HOST_ERROR",
            "RETURN_CODE": None,
            "HOST_ERROR": detail,
            "EVIDENCE_CLASS": "UNQUALIFIED_CAPTURE",
            "ACCEPTANCE_ELIGIBLE": False,
            "ACCEPTANCE_BLOCKERS": [f"HOST_ERROR: {detail}"],
            "QUALIFICATION_STATUS": "UNQUALIFIED",
            "QUALIFICATION_BLOCKERS": [f"HOST_ERROR: {detail}"],
            "TEARDOWN_CHECK": teardown,
            "RECOVERY_EVENTS": list(self.recovery_events),
        }
        self.envelopes.append(envelope)
        progress.recorded = True
        if on_case_complete is not None:
            try:
                on_case_complete(case_index, envelope)
            except (OSError, ValueError, TypeError):
                self.recovery_events.append(
                    "HOST_ERROR: the interrupted case could not be checkpointed"
                )

    def _report(self) -> dict[str, object]:
        return {
            "schema": 1,
            "mode": "campaign",
            "state": self.state,
            "terminal_reason": self.terminal_reason,
            "intervention_case_id": self.intervention_case_id,
            "resume_case_index": self.resume_case_index,
            "transport_start_problem": self.transport_start_problem,
            "hardware_lock_status": self.hardware_lock_status,
            "host_error": self.host_error,
            "firmware": self.firmware,
            "recovery_events": list(self.recovery_events),
            "envelopes": list(self.envelopes),
        }


CAMPAIGN_RESET_ESTIMATE_SECONDS = 45
HARDWARE_LOCK_ROOT = (
    ROOT.parent.parent if ROOT.parent.name.casefold() == "worktrees" else ROOT.parent
)
HARDWARE_LOCK_PATH = HARDWARE_LOCK_ROOT / "HARDWARE_LOCK.json"


def _read_campaign_plan(path: Path) -> dict[str, object]:
    try:
        plan = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"campaign plan could not be read ({type(exc).__name__})") from exc
    if not isinstance(plan, dict) or plan.get("schema") != 1:
        raise ValueError("campaign plan must be a schema 1 JSON object")
    for key in ("campaign_id", "session_id", "source_commit", "console_model",
                "host0_root", "report_path", "checkpoint_path", "cases"):
        if key not in plan:
            raise ValueError(f"campaign plan is missing {key}")
    if not isinstance(plan["campaign_id"], str) or not re.fullmatch(
        r"[A-Za-z0-9._-]{1,64}", plan["campaign_id"]
    ):
        raise ValueError("campaign_id must be a simple identifier")
    if not isinstance(plan["session_id"], str) or not re.fullmatch(
        r"[A-Za-z0-9._-]{1,64}", plan["session_id"]
    ):
        raise ValueError("session_id must be a simple identifier")
    if not isinstance(plan["source_commit"], str) or not _FULL_COMMIT_RE.fullmatch(
        plan["source_commit"]
    ):
        raise ValueError("source_commit must be a full 40- or 64-digit object id")
    if not isinstance(plan["console_model"], str) or not re.fullmatch(
        r"[A-Za-z0-9._-]{1,48}", plan["console_model"]
    ):
        raise ValueError("console_model must be a non-identifying model label")
    firmware_problem = _expected_firmware_problem(plan.get("expected_firmware"))
    if firmware_problem:
        raise ValueError(f"expected_firmware: {firmware_problem}")
    model_code = plan.get("model_code")
    if model_code is not None and (
        not isinstance(model_code, int) or isinstance(model_code, bool) or model_code < 0
    ):
        raise ValueError("model_code must be a non-negative integer raw PspModel value")
    return plan


def _campaign_plan_paths(plan_path: Path, plan: dict[str, object]):
    private_root = plan_path.resolve().parent
    paths: dict[str, Path] = {}
    for key in ("host0_root", "report_path", "checkpoint_path"):
        value = plan.get(key)
        if not isinstance(value, str) or not value:
            raise ValueError(f"{key} must be a nonempty path")
        path = Path(value)
        if not path.is_absolute():
            path = private_root / path
        path = path.resolve()
        try:
            path.relative_to(private_root)
        except ValueError as exc:
            raise ValueError(f"{key} must stay under the private plan directory") from exc
        paths[key] = path
    if not paths["host0_root"].is_dir():
        raise ValueError("host0_root must be an existing private directory")
    if not paths["report_path"].parent.is_dir() or not paths["checkpoint_path"].parent.is_dir():
        raise ValueError("report and checkpoint parent directories must already exist")
    return paths


def _campaign_plan_cases(
    plan: dict[str, object], host0_root: Path
) -> list[CampaignCase]:
    raw_cases = plan.get("cases")
    if not isinstance(raw_cases, list):
        raise ValueError("cases must be an ordered JSON array")
    case_ids: list[str] = []
    cases: list[CampaignCase] = []
    for row in raw_cases:
        if not isinstance(row, dict):
            raise ValueError("each campaign case must be a JSON object")
        case_id = row.get("case_id")
        binary_name = row.get("prx")
        timeout = row.get("timeout_seconds")
        if not isinstance(case_id, str) or not isinstance(binary_name, str):
            raise ValueError("case_id and prx must be strings")
        if not isinstance(timeout, (int, float)) or isinstance(timeout, bool) or not math.isfinite(timeout) or timeout <= 0:
            raise ValueError(f"{case_id}: timeout_seconds must be finite and positive")
        binary = (host0_root / binary_name).resolve()
        try:
            binary.relative_to(host0_root.resolve())
        except ValueError as exc:
            raise ValueError(f"{case_id}: PRX must stay inside host0_root") from exc
        if binary.suffix.lower() != ".prx" or not binary.is_file():
            raise ValueError(f"{case_id}: staged PRX is missing or not a .prx file")
        cases.append(CampaignCase(case_id, binary, float(timeout)))
        case_ids.append(case_id)
    if tuple(case_ids) != CAMPAIGN_QUEUE_CASES:
        raise ValueError("campaign cases do not match the complete ordered oracle queue")
    return cases


def _campaign_queue_summary() -> dict[str, object]:
    queue = [
        {"index": index, "case_id": case_id,
         "estimated_seconds": CAMPAIGN_CASE_ESTIMATE_SECONDS[case_id]}
        for index, case_id in enumerate(CAMPAIGN_QUEUE_CASES)
    ]
    case_seconds = sum(CAMPAIGN_CASE_ESTIMATE_SECONDS.values())
    reset_count = len(CAMPAIGN_QUEUE_CASES) - 1
    reset_seconds = reset_count * CAMPAIGN_RESET_ESTIMATE_SECONDS
    return {
        "queue": queue,
        "case_time_seconds": case_seconds,
        "reset_count": reset_count,
        "reset_time_seconds": reset_seconds,
        "estimated_total_seconds": case_seconds + reset_seconds,
    }


def _write_campaign_json(path: Path, value: dict[str, object]) -> None:
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    rendered = dump_json(value)
    temporary.write_text(rendered, encoding="utf-8")
    os.replace(temporary, path)


def _read_hardware_lock(session_id: str) -> tuple[bool, str]:
    try:
        lock = json.loads(HARDWARE_LOCK_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        return False, f"HARDWARE_LOCK_UNAVAILABLE:{type(exc).__name__}"
    if not isinstance(lock, dict) or lock.get("state") != "HELD":
        return False, "HARDWARE_LOCK_NOT_HELD"
    if lock.get("power_cycle_confirmed") is not True:
        return False, "HARDWARE_LOCK_POWER_CYCLE_NOT_CONFIRMED"
    if lock.get("holder_session") != session_id:
        return False, "HARDWARE_LOCK_SESSION_MISMATCH"
    return True, "HELD_AND_CONFIRMED"


_SESSION_ID_RE = re.compile(r"[A-Za-z0-9._-]{1,64}")


class HardwareLockError(RuntimeError):
    """The maintainer's hardware lock does not authorise this session to touch the PSP."""

    def __init__(self, status: str) -> None:
        super().__init__(status)
        self.status = status


def require_hardware_lock(session_id: object) -> None:
    """The one gate every PSP-touching path passes, re-read from disk on each call.

    Callers: transport start (and so every L1 restart), each case before its
    soft reset and again before its launch, every PSPLink ``reset``, a raw
    ``--command`` capture, and the campaign plan before it writes a checkpoint.
    The lock must be HELD with a confirmed power cycle by exactly this session.
    """

    if not isinstance(session_id, str) or not _SESSION_ID_RE.fullmatch(session_id):
        raise HardwareLockError("HARDWARE_LOCK_SESSION_REQUIRED")
    held, status = _read_hardware_lock(session_id)
    if not held:
        raise HardwareLockError(status)


def _checkpoint_waiting(
    state: dict[str, object], failed_case_id: str | None, resume_case_index: int
) -> dict[str, object]:
    """Return ``state`` stopped for a maintainer power cycle at a known position."""

    interrupted = [str(item) for item in state.get("interrupted_cases", [])]
    if failed_case_id is not None and failed_case_id not in interrupted:
        interrupted.append(failed_case_id)
    return {
        **state,
        "state": "WAITING_FOR_POWER_CYCLE",
        "failed_case_id": failed_case_id,
        "interrupted_cases": interrupted,
        "next_case_index": resume_case_index,
        "active_case_index": None,
        "active_case_id": None,
        "phase": None,
    }


def _checkpoint_preflight_position(state: dict[str, object], resume_case_index: int) -> int:
    """Resume past the host0 preflight only after it qualified host0."""

    return resume_case_index if state.get("host0_qualified") is True else 0


def _checkpoint_case_started(
    state: dict[str, object], index: int, case_id: str, phase: str
) -> dict[str, object]:
    """Durable record written before a reset (RESET_BEFORE_CASE) or a launch (CASE_ACTIVE)."""

    if phase not in {"RESET_BEFORE_CASE", "CASE_ACTIVE"}:
        raise ValueError(f"unknown campaign case phase {phase!r}")
    return {
        **state,
        "state": "RUNNING",
        "active_case_index": index,
        "active_case_id": case_id,
        "phase": phase,
        "next_case_index": index,
    }


def _checkpoint_case_finished(
    state: dict[str, object],
    index: int,
    case_id: str,
    *,
    intervention_case_id: str | None,
    resume_case_index: int | None,
    host0_qualified: bool,
    power_cycle_required: bool = True,
) -> dict[str, object]:
    """Durable record written after a launched case's capture and teardown check.

    ``power_cycle_required=False`` is the one interrupted outcome that needs no
    power cycle: a host-side error ended the case but its teardown was then
    verified clean. The case is recorded as interrupted, not completed.
    """

    updated = {**state, "host0_qualified": host0_qualified}
    if intervention_case_id is not None:
        resume = _checkpoint_preflight_position(
            updated, index + 1 if resume_case_index is None else resume_case_index
        )
        if power_cycle_required:
            return _checkpoint_waiting(updated, intervention_case_id, resume)
        interrupted = [str(item) for item in updated.get("interrupted_cases", [])]
        if intervention_case_id not in interrupted:
            interrupted.append(intervention_case_id)
        return {
            **updated,
            "state": "IN_PROGRESS",
            "failed_case_id": intervention_case_id,
            "interrupted_cases": interrupted,
            "next_case_index": resume,
            "active_case_index": None,
            "active_case_id": None,
            "phase": None,
        }
    completed = [str(item) for item in updated.get("completed_cases", [])]
    completed.append(case_id)
    return {
        **updated,
        "state": "IN_PROGRESS",
        "completed_cases": completed,
        "next_case_index": index + 1,
        "active_case_index": None,
        "active_case_id": None,
        "phase": None,
    }


def _checkpoint_after_run(
    state: dict[str, object],
    *,
    terminal_reason: str | None,
    intervention_case_id: str | None,
    resume_case_index: int | None,
    case_count: int,
) -> dict[str, object]:
    """Durable record once the runner has returned.

    The resume position is never discarded. A stop demands a power cycle only
    when the PSP may have been left running something: the runner asked for
    physical intervention, or a launched case (phase ``CASE_ACTIVE``) has no
    completion record. A stop before the active case launched, including a
    transport start failure, keeps ``IN_PROGRESS`` at that case.
    """

    if state.get("state") == "WAITING_FOR_POWER_CYCLE":
        return dict(state)  # the per-case record already holds the resume position
    running = state.get("state") == "RUNNING"
    active_index = state.get("active_case_index") if running else None
    if running and not isinstance(active_index, int):
        raise ValueError("a running campaign checkpoint must name its active case index")
    position = active_index if running else state.get("next_case_index")
    if not isinstance(position, int):
        raise ValueError("campaign checkpoint has no resume position")
    launched = running and state.get("phase") == "CASE_ACTIVE"

    if terminal_reason is None and not running:
        if position == case_count:
            return {
                **state,
                "state": "COMPLETE",
                "active_case_index": None,
                "active_case_id": None,
                "phase": None,
            }
        return dict(state)
    if terminal_reason == "PHYSICAL_INTERVENTION_REQUIRED" or launched:
        if resume_case_index is None:
            resume_case_index = position + 1 if launched else position
        failed = intervention_case_id
        if failed is None and launched:
            failed = str(state.get("active_case_id"))
        return _checkpoint_waiting(
            state, failed, _checkpoint_preflight_position(state, resume_case_index)
        )
    return {
        **state,
        "state": "IN_PROGRESS",
        "next_case_index": position,
        "active_case_index": None,
        "active_case_id": None,
        "phase": None,
    }


def _checkpoint_resume(
    checkpoint: dict[str, object] | None,
    *,
    confirm_power_cycle: bool,
    case_count: int,
) -> tuple[int, dict[str, object]] | dict[str, object]:
    """Return ``(start_index, carried fields)`` for a run, or a refusal/status report."""

    carried: dict[str, object] = {
        "completed_cases": [],
        "interrupted_cases": [],
        "host0_qualified": False,
    }
    if checkpoint is None:
        if confirm_power_cycle:
            return {"status": "REFUSED", "reason": "no interrupted campaign needs confirmation"}
        return 0, carried

    carried = {
        "completed_cases": list(checkpoint.get("completed_cases", [])),
        "interrupted_cases": list(checkpoint.get("interrupted_cases", [])),
        "host0_qualified": checkpoint.get("host0_qualified") is True,
    }
    state = checkpoint.get("state")
    if state == "COMPLETE":
        return {"status": "COMPLETE", "case_count": case_count}
    if state == "RUNNING":
        if not confirm_power_cycle:
            return {
                "status": "WAITING_FOR_POWER_CYCLE_CONFIRMATION",
                "reason": "previous launch stopped before a durable case completion",
                "active_case_id": checkpoint.get("active_case_id"),
            }
        phase = checkpoint.get("phase")
        active_index = checkpoint.get("active_case_index")
        if not isinstance(active_index, int) or not 0 <= active_index < case_count:
            return {"status": "REFUSED", "reason": "invalid active case index"}
        if phase not in {"RESET_BEFORE_CASE", "CASE_ACTIVE"}:
            return {"status": "REFUSED", "reason": "invalid active case phase"}
        start_index = active_index + 1 if phase == "CASE_ACTIVE" else active_index
    elif state == "WAITING_FOR_POWER_CYCLE":
        if not confirm_power_cycle:
            return {
                "status": "WAITING_FOR_POWER_CYCLE_CONFIRMATION",
                "failed_case_id": checkpoint.get("failed_case_id"),
                "resume_case_index": checkpoint.get("next_case_index"),
            }
        start_index = checkpoint.get("next_case_index")
    elif state == "IN_PROGRESS":
        if confirm_power_cycle:
            return {
                "status": "REFUSED",
                "reason": (
                    "power-cycle confirmation is only accepted after an interrupted case; "
                    "this checkpoint resumes without it at case index "
                    f"{checkpoint.get('next_case_index')}"
                ),
            }
        start_index = checkpoint.get("next_case_index")
    else:
        return {"status": "REFUSED", "reason": "unknown checkpoint state"}
    if (
        not isinstance(start_index, int) or isinstance(start_index, bool)
        or not 0 <= start_index <= case_count
    ):
        return {"status": "REFUSED", "reason": "invalid resume case index"}
    if 0 < start_index < case_count and carried["host0_qualified"] is not True:
        return {
            "status": "REFUSED",
            "reason": (
                "checkpoint does not record a qualified transport-write host0 preflight; "
                "remove the checkpoint to restart the campaign at transport-write"
            ),
        }
    return start_index, carried


def run_campaign_plan(
    plan_path: Path,
    *,
    dry_run: bool,
    confirm_power_cycle: bool,
    pspsh_argv: list[str],
    usbhostfs_argv: list[str],
    transport_factory: Callable[..., object] | None = None,
) -> tuple[int, dict[str, object]]:
    """Validate or execute the resumable, one-launch-per-boot campaign queue."""

    try:
        plan = _read_campaign_plan(plan_path)
        paths = _campaign_plan_paths(plan_path, plan)
        cases = _campaign_plan_cases(plan, paths["host0_root"])
    except ValueError as exc:
        return 2, {"status": "REFUSED", "reason": str(exc)}

    summary = _campaign_queue_summary()
    if dry_run:
        source_problem = _check_source_tree(str(plan["source_commit"]))
        if source_problem:
            return 2, {"status": "REFUSED", "reason": source_problem, **summary}
        return 0, {
            "status": "VALIDATED_OFFLINE",
            "campaign_id": plan["campaign_id"],
            "case_count": len(cases),
            "hardware_started": False,
            **summary,
        }

    try:
        require_hardware_lock(plan["session_id"])
    except HardwareLockError as exc:
        return 2, {"status": "REFUSED", "reason": exc.status, **summary}

    checkpoint_path = paths["checkpoint_path"]
    checkpoint: dict[str, object] | None = None
    if checkpoint_path.is_file():
        try:
            checkpoint = json.loads(checkpoint_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            return 2, {
                "status": "REFUSED",
                "reason": f"campaign checkpoint is unreadable ({type(exc).__name__})",
                **summary,
            }
        if not isinstance(checkpoint, dict) or any(
            checkpoint.get(key) != expected
            for key, expected in (
                ("campaign_id", plan["campaign_id"]),
                ("source_commit", plan["source_commit"]),
                ("session_id", plan["session_id"]),
                ("queue", list(CAMPAIGN_QUEUE_CASES)),
            )
        ):
            return 2, {"status": "REFUSED", "reason": "checkpoint identity mismatch", **summary}

    resume = _checkpoint_resume(
        checkpoint, confirm_power_cycle=confirm_power_cycle, case_count=len(cases)
    )
    if isinstance(resume, dict):
        return (0 if resume.get("status") == "COMPLETE" else 2), {**resume, **summary}
    start_index, carried = resume
    if start_index >= len(cases):
        return 0, {"status": "COMPLETE", "case_count": len(cases), **summary}

    state: dict[str, object] = {
        "schema": 1,
        "campaign_id": plan["campaign_id"],
        "source_commit": plan["source_commit"],
        "session_id": plan["session_id"],
        "queue": list(CAMPAIGN_QUEUE_CASES),
        "state": "IN_PROGRESS",
        **carried,
        "next_case_index": start_index,
        "active_case_index": None,
        "active_case_id": None,
        "phase": None,
        "failed_case_id": None,
    }
    _write_campaign_json(checkpoint_path, state)
    transport = (transport_factory or PsplinkProcessTransport)(
        pspsh_argv=pspsh_argv,
        usbhostfs_argv=usbhostfs_argv,
        host0_root=paths["host0_root"],
        session_id=str(plan["session_id"]),
    )
    runner = PsplinkCampaignRunner(
        transport,
        console_model=str(plan["console_model"]),
        source_commit=str(plan["source_commit"]),
        model_code=plan.get("model_code"),
        expected_firmware=plan.get("expected_firmware"),
    )
    runner.host0_qualified = start_index > 0 and carried["host0_qualified"] is True
    remaining = cases[start_index:]

    def record_start(index: int, case: CampaignCase, phase: str) -> None:
        state.update(_checkpoint_case_started(state, index, case.case_id, phase))
        _write_campaign_json(checkpoint_path, state)

    def record_complete(index: int, envelope: dict[str, object]) -> None:
        state.update(_checkpoint_case_finished(
            state,
            index,
            str(envelope["CASE_ID"]),
            intervention_case_id=runner.intervention_case_id,
            resume_case_index=runner.resume_case_index,
            host0_qualified=runner.host0_qualified,
            power_cycle_required=runner.intervention_requires_power_cycle,
        ))
        _write_campaign_json(checkpoint_path, state)

    try:
        report = runner.run(
            remaining,
            require_transport_preflight=start_index == 0,
            reset_between_cases=True,
            stop_on_incomplete=True,
            case_index_offset=start_index,
            on_case_start=record_start,
            on_case_complete=record_complete,
        )
    except Exception as exc:  # noqa: BLE001 - finalize the checkpoint whatever happened
        try:
            transport.stop()
        except Exception:  # noqa: BLE001 - the original error is the one to report
            pass
        runner.host_error = f"campaign runner: {_host_error_summary(exc)}"
        runner.recovery_events.append(f"HOST_ERROR: {runner.host_error}")
        runner.state = "STOPPED"
        if runner.terminal_reason != "PHYSICAL_INTERVENTION_REQUIRED":
            runner.terminal_reason = "HOST_ERROR"
        report = runner._report()
    final_state = _checkpoint_after_run(
        state,
        terminal_reason=report.get("terminal_reason"),
        intervention_case_id=runner.intervention_case_id,
        resume_case_index=runner.resume_case_index,
        case_count=len(cases),
    )
    if final_state != state:
        state = final_state
        _write_campaign_json(checkpoint_path, state)

    report.update({
        "campaign_id": plan["campaign_id"],
        "queue_case_count": len(cases),
        "start_case_index": start_index,
        "checkpoint_state": state.get("state"),
        "checkpoint_next_case_index": state.get("next_case_index"),
        **summary,
    })
    _write_campaign_json(paths["report_path"], report)
    return (3 if report.get("terminal_reason") == "PHYSICAL_INTERVENTION_REQUIRED" else
            2 if report.get("terminal_reason") else
            0 if state.get("state") == "COMPLETE" else 2), report


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


def _host0_file_identity(info: os.stat_result) -> tuple[int, int]:
    return info.st_dev, info.st_ino


def _read_host0_file(path: Path) -> tuple[bytes, os.stat_result]:
    """Read one regular file through a descriptor bound to its checked path."""

    before = path.lstat()
    if not stat.S_ISREG(before.st_mode):
        raise UnsafeHost0OutputError(
            f"host0 output is not a regular file: {path.name}"
        )

    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    try:
        opened = os.fstat(descriptor)
        if (
            not stat.S_ISREG(opened.st_mode)
            or _host0_file_identity(before) != _host0_file_identity(opened)
        ):
            raise UnsafeHost0OutputError(
                f"host0 output changed during capture: {path.name}"
            )

        stream = os.fdopen(descriptor, "rb")
        descriptor = -1
        with stream:
            raw = stream.read()
            after_read = os.fstat(stream.fileno())

        try:
            after_path = path.lstat()
        except FileNotFoundError as exc:
            raise UnsafeHost0OutputError(
                f"host0 output changed during capture: {path.name}"
            ) from exc
        if (
            not stat.S_ISREG(after_path.st_mode)
            or _host0_file_identity(opened) != _host0_file_identity(after_read)
            or _host0_file_identity(after_read) != _host0_file_identity(after_path)
        ):
            raise UnsafeHost0OutputError(
                f"host0 output changed during capture: {path.name}"
            )
        return raw, after_path
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _wait_for_host0_output(
    path: Path,
    timeout: float,
    *,
    not_before_ns: int | None = None,
    ready: Callable[[str], bool],
    include_mtime: bool = False,
) -> str | tuple[str, int]:
    """Read a probe-owned host0 file after its complete stream is available."""

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            first, first_stat = _read_host0_file(path)
            if (
                first_stat.st_size > 0
                and (not_before_ns is None or first_stat.st_mtime_ns >= not_before_ns)
            ):
                time.sleep(0.05)
                second, second_stat = _read_host0_file(path)
                if _host0_file_identity(first_stat) != _host0_file_identity(second_stat):
                    raise UnsafeHost0OutputError(
                        f"host0 output changed during capture: {path.name}"
                    )
                if (
                    first == second
                    and len(first) == first_stat.st_size
                    and len(second) == second_stat.st_size
                    and first_stat.st_size == second_stat.st_size
                    and first_stat.st_mtime_ns == second_stat.st_mtime_ns
                ):
                    stable = first.decode("utf-8", errors="replace")
                    if (
                        (not_before_ns is None or first_stat.st_mtime_ns >= not_before_ns)
                        and (not_before_ns is None or second_stat.st_mtime_ns >= not_before_ns)
                        and ready(stable)
                    ):
                        if include_mtime:
                            return stable, second_stat.st_mtime_ns
                        return stable
        except UnsafeHost0OutputError:
            raise
        except OSError:
            pass
        time.sleep(0.1)
    raise TimeoutError(f"host0 output did not become complete: {path.name}")


_STEP_LINE_RE = re.compile(
    r"^NAKAGAWA_PSP_STEP schema=1 case_id=\S+ step=(\S+)\s*$", re.MULTILINE
)


def _host0_progress_detail(
    text: str | None, mtime_ns: int | None, run_started_ns: int
) -> str:
    """Describe how far an unfinished host0 stream got, for the stop reason."""

    if text is None or mtime_ns is None:
        return "no host0 output was observed"
    records = sum(line.startswith("NAKAGAWA_PSP_TEST ") for line in text.splitlines())
    steps = _STEP_LINE_RE.findall(text)
    detail = (
        f"{records} result record(s); last host0 write "
        f"{max(mtime_ns - run_started_ns, 0) / 1e9:.1f}s after launch"
    )
    return detail + (f"; last step marker: {steps[-1]}" if steps else "")


def _snapshot_host0_output(path: Path) -> tuple[str | None, int | None, str | None]:
    """Capture bounded diagnostics without following a link or special file."""

    try:
        raw, info = _read_host0_file(path)
    except FileNotFoundError:
        return None, None, "per-case host0 log was not produced before probe unload"
    except UnsafeHost0OutputError as exc:
        return None, None, str(exc)
    except OSError as exc:
        return None, None, f"per-case host0 log could not be inspected ({type(exc).__name__})"
    try:
        text = raw.decode("utf-8")
    except UnicodeError as exc:
        return None, info.st_mtime_ns, (
            f"per-case host0 log is not valid UTF-8 ({type(exc).__name__})"
        )
    return text, info.st_mtime_ns, None


def _host0_capture_complete(text: str, args: argparse.Namespace) -> bool:
    try:
        return len(parse_output(_canonicalize_psp(text, args)).results) == 16
    except (ProtocolError, OSError, ValueError):
        return False


def _validate_host0_capture(text: str, args: argparse.Namespace) -> dict[str, object]:
    """Validate the complete DMAC host0 stream without promoting placeholders."""

    parsed = validate_dmac_size_matrix(_canonicalize_psp(text, args))
    metadata = parsed.metadata_dict()
    identity = _device_identity(text, args)
    blockers = list(provenance_issues(metadata)) + list(identity["DEVICE_IDENTITY_BLOCKERS"])
    return {
        "classification": "PASS" if all(result.status == "PASS" for result in parsed.results) else "FAIL",
        "test_record_count": len(parsed.results),
        "metadata": metadata,
        "acceptance_eligible": not blockers,
        "acceptance_blockers": blockers,
        **identity,
    }


def annotate_terminal_outcome(
    report: dict[str, object], capture: bytes, outcome: str
) -> dict[str, object]:
    """Attach a human-observed HANG/RESET label to a no-record capture.

    A host process exit cannot distinguish a device reset from a probe hang.
    The annotation is therefore explicit human evidence, never an inference,
    and it is rejected when the PSP already emitted any scalar test record.
    It also records the capture's last complete STEP marker, which names where
    the probe stopped.
    """

    if outcome not in TERMINAL_OUTCOMES:
        raise ValueError(f"unsupported terminal outcome: {outcome}")
    text = capture.decode("utf-8", errors="replace")
    classification, record_count = _record_summary(text)
    if record_count:
        raise ValueError("terminal outcome annotation requires a capture with no test records")
    last_step = parse_progress(text).last_step
    annotated = dict(report)
    annotated["record_classification"] = classification
    annotated["test_record_count"] = 0
    annotated["terminal_outcome"] = outcome
    annotated["terminal_outcome_source"] = "human-observed"
    annotated["last_probe_step"] = (
        None if last_step is None
        else {"case_id": last_step.case_id, "step": last_step.step}
    )
    annotated["capture_sha256"] = hashlib.sha256(capture).hexdigest()
    annotated["acceptance_eligible"] = False
    annotated["acceptance_blockers"] = [
        "terminal outcome has no scalar PSP result stream",
        "human observation must be accompanied by model/firmware/CFW/clock in the hardware handoff",
    ]
    return annotated


PROVENANCE_FLAGS = ("binary", "source_commit", "model", "firmware")


_IDENTITY_MATCH = "MATCH"
_IDENTITY_MISMATCH = "MISMATCH"
_IDENTITY_PLACEHOLDER = "PLACEHOLDER"
_IDENTITY_NOT_REPORTED = "NOT_REPORTED"
_IDENTITY_NOT_BOUND = "NOT_BOUND"
_IDENTITY_PARTIAL = "PARTIAL"


def _is_measured_identity(value: str | None, value_re: re.Pattern[str]) -> bool:
    return bool(
        value
        and value_re.fullmatch(value)
        and not _ALL_ZERO_RE.fullmatch(value)
    )


def _host_binary_sha256(binary: Path) -> str:
    """Hash the staged PRX as a host measurement."""

    digest = hashlib.sha256()
    with binary.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _device_metadata(text: str) -> dict[str, str]:
    """Read the first raw META record without treating it as host evidence."""

    for line in text.splitlines():
        if not line.startswith("NAKAGAWA_PSP_META "):
            continue
        fields: dict[str, str] = {}
        for token in line.split()[1:]:
            key, separator, value = token.partition("=")
            if separator:
                fields[key] = value
        return fields
    return {}


def _device_metadata_equal(left: dict[str, str], right: dict[str, str]) -> bool:
    if left.keys() != right.keys():
        return False
    return all(
        left[key].casefold() == right[key].casefold()
        if key in {"source_commit", "binary_sha256"}
        else left[key] == right[key]
        for key in left
    )


def _identity_binding(
    reported: str | None,
    expected: str | None,
    field: str,
    value_re: re.Pattern[str],
    full_shape: str,
) -> tuple[str, list[str]]:
    """Classify one device-reported field against the full host expectation."""

    if expected is None:
        return _IDENTITY_NOT_BOUND, []
    if not _is_measured_identity(expected, value_re):
        return _IDENTITY_NOT_BOUND, [
            f"IDENTITY_NOT_BOUND: host-expected {field} is not a full {full_shape} value"
        ]
    if reported is None:
        return _IDENTITY_NOT_REPORTED, [
            f"IDENTITY_NOT_BOUND: the device reported no {field}, so the capture "
            "is not bound to the host-expected build"
        ]
    if _ALL_ZERO_RE.fullmatch(reported):
        return _IDENTITY_PLACEHOLDER, [
            f"IDENTITY_NOT_BOUND: the device did not report its own build identity; "
            f"{field} is the all-zero placeholder"
        ]
    if not value_re.fullmatch(reported):
        return _IDENTITY_NOT_BOUND, [
            f"IDENTITY_NOT_BOUND: device {field} {reported!r} must be a full "
            f"{full_shape}; a short prefix never binds"
        ]
    if reported.casefold() != expected.casefold():
        return _IDENTITY_MISMATCH, [
            f"IDENTITY_MISMATCH: device {field} {reported!r} does not match the "
            f"host-expected {expected!r}"
        ]
    return _IDENTITY_MATCH, []


def _combine_identity(commit_status: str, digest_status: str) -> str:
    if _IDENTITY_MISMATCH in (commit_status, digest_status):
        return _IDENTITY_MISMATCH
    if commit_status in {
        _IDENTITY_NOT_BOUND,
        _IDENTITY_NOT_REPORTED,
        _IDENTITY_PLACEHOLDER,
    }:
        return commit_status
    if commit_status == _IDENTITY_MATCH and digest_status == _IDENTITY_MATCH:
        return _IDENTITY_MATCH
    return _IDENTITY_PARTIAL


def _device_identity(text: str, args: argparse.Namespace) -> dict[str, object]:
    """Compare raw device identity before host canonicalization."""

    device = _device_metadata(text)
    binary = getattr(args, "binary", None)
    try:
        expected_digest = _host_binary_sha256(binary) if isinstance(binary, Path) else None
    except OSError:
        expected_digest = None
    expected_commit = getattr(args, "source_commit", None)
    commit_status, commit_problems = _identity_binding(
        device.get("source_commit"),
        expected_commit,
        "source_commit",
        _FULL_COMMIT_RE,
        "40- or 64-digit hexadecimal object id",
    )
    digest_status, digest_problems = _identity_binding(
        device.get("binary_sha256"),
        expected_digest,
        "binary_sha256",
        _FULL_SHA256_RE,
        "64-digit hexadecimal digest",
    )
    blockers = list(commit_problems) if expected_commit is not None else []
    if digest_status == _IDENTITY_MISMATCH:
        blockers.extend(digest_problems)
    return {
        "DEVICE_REPORTED_SOURCE_COMMIT": device.get("source_commit"),
        "DEVICE_REPORTED_BINARY_SHA256": device.get("binary_sha256"),
        "SOURCE_COMMIT_BINDING": commit_status,
        "BINARY_SHA256_BINDING": digest_status,
        "DEVICE_IDENTITY_STATUS": _combine_identity(commit_status, digest_status),
        "DEVICE_IDENTITY_BLOCKERS": blockers,
    }


def _canonicalize_psp(text: str, args: argparse.Namespace) -> str:
    """Fill unmeasured fields while preserving any device identity claim.

    Full device object ids are compared case-insensitively after validating that
    they contain exactly 40 or 64 hexadecimal digits. Abbreviated prefixes never
    match. An all-zero or absent device commit may be replaced in the parsed view
    by the host expectation, but ``_device_identity`` records that it was not
    device-bound and closes acceptance.

    A supplied model code is checked against the model-profile row when present;
    the row's exact ``out0`` spelling remains authoritative in the metadata.
    """

    parsed = parse_output(_normalise_unbound_identity_fields(text))
    device_model_raw_value = _model_profile_raw_value(parsed)
    model_code = getattr(args, "model_code", None)
    _validate_model_code_expectation(device_model_raw_value, model_code)

    if not any(getattr(args, flag, None) for flag in PROVENANCE_FLAGS):
        return text
    metadata_lines = [
        line for line in text.splitlines() if line.startswith("NAKAGAWA_PSP_META ")
    ]
    if len(metadata_lines) != 1:
        return text

    device = _device_metadata(text)
    model = getattr(args, "model", None) or device.get("model", "unknown")
    firmware = getattr(args, "firmware", None) or device.get("firmware", "unknown")
    model_fields = f"model={model}"
    raw_model_code = (
        device_model_raw_value
        if device_model_raw_value is not None
        else str(model_code) if model_code is not None else None
    )
    if raw_model_code is not None:
        model_fields += f" model_code={raw_model_code}"
        try:
            generation, _retail = decode_psp_model_code(int(raw_model_code, 0))
        except ValueError:
            pass
        else:
            model_fields += f" model_generation={generation}"

    reported_commit = device.get("source_commit")
    if _is_measured_identity(reported_commit, _FULL_COMMIT_RE):
        source_commit = reported_commit.lower()
    else:
        source_commit = getattr(args, "source_commit", None) or reported_commit or ""

    reported_digest = device.get("binary_sha256")
    if _is_measured_identity(reported_digest, _FULL_SHA256_RE):
        binary_sha256 = reported_digest.lower()
    else:
        binary = getattr(args, "binary", None)
        binary_sha256 = (
            _host_binary_sha256(binary)
            if isinstance(binary, Path)
            else reported_digest or ""
        )

    metadata = (
        "NAKAGAWA_PSP_META schema=1 source=psp "
        f"{model_fields} firmware={firmware} "
        f"binary_sha256={binary_sha256} source_commit={source_commit}"
    )
    records = [line for line in text.splitlines() if not line.startswith("NAKAGAWA_PSP_META ")]
    return metadata + "\n" + "\n".join(records) + "\n"


def _argv_json(parser: argparse.ArgumentParser, value: str, option: str) -> list[str]:
    try:
        parsed = json.loads(value)
    except json.JSONDecodeError as exc:
        parser.error(f"{option} must be a JSON string array: {exc.msg}")
    if not isinstance(parsed, list) or not parsed or not all(
        isinstance(item, str) and item for item in parsed
    ):
        parser.error(f"{option} must be a non-empty JSON string array")
    return parsed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--pspsh", default="pspsh", help="explicit pspsh executable name/path")
    parser.add_argument("--prx", help="source-owned PRX to launch")
    parser.add_argument("--remote-command", help="explicit pspsh command accepted by the installed build")
    parser.add_argument("--command", help="host command to run for capture (no shell expansion)")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument(
        "--shell-verification-timeout",
        type=float,
        default=DEFAULT_SHELL_VERIFICATION_TIMEOUT,
        help="total deadline in seconds for up to three PSPLink `ver` attempts (default: 45)",
    )
    parser.add_argument("--psp-output", type=Path)
    parser.add_argument("--nakagawa-output", type=Path)
    parser.add_argument(
        "--results-directory",
        type=Path,
        default=DEFAULT_RESULTS,
        help="directory inside the repository for this run's captures and report inputs",
    )
    parser.add_argument(
        "--host0-output",
        type=Path,
        help="local host0-mapped DMAC matrix file to retain and validate after launch",
    )
    parser.add_argument(
        "--validate-dmac-size-matrix",
        action="store_true",
        help="validate --host0-output as the complete PSP-DMAC-001 size matrix",
    )
    parser.add_argument(
        "--ge-corpus-gate",
        action="store_true",
        help="validate the source-owned GE corpus and report per-case evidence status without hardware access",
    )
    parser.add_argument("--ge-corpus", type=Path, help="GE corpus JSON under fixtures/psp_oracle")
    parser.add_argument("--ge-corpus-schema", type=Path, help="GE corpus schema JSON under assets")
    parser.add_argument("--binary", type=Path, help="source-owned PRX used to replace fixture metadata")
    parser.add_argument("--source-commit", help="exact source commit recorded in the result metadata")
    parser.add_argument("--model", help="operator-declared physical PSP model label")
    parser.add_argument(
        "--model-code",
        type=lambda value: int(value, 0),
        help=(
            "raw PSPSDK/kubridge PspModel ordinal returned by the device; interpreted "
            "separately from --model (do not use for sceKernelGetModel's original/slim return)"
        ),
    )
    parser.add_argument(
        "--firmware",
        help=(
            "human-recorded PSP firmware identifier; with --campaign-case it is the "
            "expected PSPLink `pspver` version, for example 6.6.1 for firmware 6.61"
        ),
    )
    parser.add_argument(
        "--campaign-case",
        action="append",
        default=[],
        metavar="CASE_ID=PRX_PATH",
        help="run an existing source-owned PRX from the host0 root; may be repeated",
    )
    parser.add_argument(
        "--campaign-plan",
        type=Path,
        help="execute or validate a private, resumable complete oracle campaign plan",
    )
    parser.add_argument(
        "--confirm-power-cycle",
        action="store_true",
        help="confirm a maintainer power cycle before resuming after an interrupted case",
    )
    parser.add_argument("--host0-root", type=Path, help="scratch directory shared by usbhostfs_pc")
    parser.add_argument(
        "--session-id",
        help=(
            "hardware lock holder session; required by --campaign-case and --command, "
            "which touch the PSP (a campaign plan names its own session_id)"
        ),
    )
    parser.add_argument(
        "--pspsh-argv-json",
        default='["pspsh", "-e", "{remote_command}"]',
        help="JSON argv template for pspsh; {remote_command} receives one shell command",
    )
    parser.add_argument(
        "--usbhostfs-argv-json",
        default='["usbhostfs_pc", "{host0_root}"]',
        help="JSON argv template for usbhostfs_pc; supports {host0_root} and {host0_root_wsl}",
    )
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
    if args.ge_corpus_gate:
        if any((
            args.prx, args.remote_command, args.command, args.psp_output, args.nakagawa_output,
            args.host0_output, args.validate_dmac_size_matrix, args.binary, args.source_commit,
            args.model, args.model_code is not None, args.firmware, args.campaign_case,
            args.campaign_plan, args.confirm_power_cycle,
            args.host0_root, args.out, args.annotate_report, args.observed_terminal_outcome,
            args.dry_run,
        )):
            parser.error("--ge-corpus-gate cannot be combined with PSPLink execution or capture options")
        corpus_path = (args.ge_corpus or DEFAULT_GE_CORPUS).resolve()
        schema_path = (args.ge_corpus_schema or DEFAULT_GE_CORPUS_SCHEMA).resolve()
        allowed_corpus_root = (ROOT / "fixtures" / "psp_oracle").resolve()
        allowed_schema_root = (ROOT / "assets").resolve()
        try:
            corpus_path.relative_to(allowed_corpus_root)
        except ValueError:
            parser.error("--ge-corpus must stay under fixtures/psp_oracle")
        try:
            schema_path.relative_to(allowed_schema_root)
        except ValueError:
            parser.error("--ge-corpus-schema must stay under assets")
        # The identity markers alone cannot prove the contract content, so the gate only
        # accepts the tracked schema file itself.
        if schema_path != (allowed_schema_root / "ge_corpus.schema.json").resolve():
            parser.error("--ge-corpus-schema must be assets/ge_corpus.schema.json")
        try:
            corpus = json.loads(corpus_path.read_text(encoding="utf-8"))
            schema = json.loads(schema_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            report = {
                "status": "REFUSED",
                "semantic_boundary": "GE_RASTER_PIXEL_CONFORMANCE",
                "tracking_issue": 343,
                "cases": [{
                    "case_id": "<corpus>",
                    "status": "REFUSED",
                    "reason": f"could not read GE corpus or schema: {type(exc).__name__}: {exc}",
                }],
            }
            sys.stdout.write(dump_json(report))
            return 2
        results_root = args.results_directory.resolve()
        try:
            results_root.relative_to(ROOT.resolve())
        except ValueError:
            parser.error("--results-directory must be inside the repository root")
        report = ge_corpus_report(corpus, schema, results_root if results_root.is_dir() else None)
        sys.stdout.write(dump_json(report))
        return 2 if report["status"] == "REFUSED" else 0

    if args.campaign_plan:
        if any((
            args.prx, args.remote_command, args.command, args.psp_output,
            args.nakagawa_output, args.host0_output, args.validate_dmac_size_matrix,
            args.binary, args.source_commit, args.model, args.model_code is not None,
            args.firmware, args.campaign_case, args.host0_root, args.out,
            args.annotate_report, args.observed_terminal_outcome, args.session_id,
        )):
            parser.error("campaign-plan mode cannot be combined with single-run or manual campaign options")
        if args.confirm_power_cycle and args.dry_run:
            parser.error("--confirm-power-cycle cannot be combined with --dry-run")
        try:
            pspsh_argv = _argv_json(parser, args.pspsh_argv_json, "--pspsh-argv-json")
            usbhostfs_argv = _argv_json(
                parser, args.usbhostfs_argv_json, "--usbhostfs-argv-json"
            )
        except SystemExit:
            raise
        code, report = run_campaign_plan(
            args.campaign_plan.resolve(),
            dry_run=args.dry_run,
            confirm_power_cycle=args.confirm_power_cycle,
            pspsh_argv=pspsh_argv,
            usbhostfs_argv=usbhostfs_argv,
        )
        sys.stdout.write(dump_json(report))
        return code
    if args.confirm_power_cycle:
        parser.error("--confirm-power-cycle requires --campaign-plan")

    args.results_directory = args.results_directory.resolve()
    try:
        args.results_directory.relative_to(ROOT.resolve())
    except ValueError:
        parser.error("--results-directory must be inside the repository root")

    if args.campaign_case:
        if (
            args.command or args.annotate_report or args.psp_output or args.nakagawa_output
            or args.host0_output or args.dry_run or args.validate_dmac_size_matrix
        ):
            parser.error("campaign mode cannot be combined with single-capture or annotation options")
        if not args.host0_root or not args.model or not args.source_commit:
            parser.error("campaign mode requires --host0-root, operator-declared --model, and --source-commit")
        if not args.session_id or not _SESSION_ID_RE.fullmatch(args.session_id):
            parser.error("campaign mode touches the PSP and requires --session-id naming the hardware lock holder")
        firmware_problem = _expected_firmware_problem(args.firmware)
        if firmware_problem:
            parser.error(f"--firmware: {firmware_problem}")
        if not _FULL_COMMIT_RE.fullmatch(args.source_commit):
            parser.error("--source-commit must be a full 40- or 64-digit object id")
        if not re.fullmatch(r"[A-Za-z0-9._-]{1,48}", args.model):
            parser.error("--model must be a non-identifying label using letters, digits, dot, _ or -")
        if not math.isfinite(args.timeout) or args.timeout <= 0:
            parser.error("--timeout must be finite and positive")
        if not math.isfinite(args.shell_verification_timeout) or args.shell_verification_timeout <= 0:
            parser.error("--shell-verification-timeout must be finite and positive")
        host0_root = args.host0_root.resolve()
        fixture_root = (ROOT / "fixtures" / "psp_oracle").resolve()
        try:
            host0_root.relative_to(fixture_root)
        except ValueError:
            parser.error("--host0-root must be a scratch directory under fixtures/psp_oracle")
        if not host0_root.is_dir():
            parser.error("--host0-root must be an existing scratch directory")
        cases: list[CampaignCase] = []
        seen_case_ids: set[str] = set()
        for specification in args.campaign_case:
            case_id, separator, raw_path = specification.partition("=")
            if not separator or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", case_id):
                parser.error("--campaign-case must be CASE_ID=PRX_PATH with a simple case id")
            if case_id in seen_case_ids:
                parser.error(f"duplicate campaign case id: {case_id}")
            seen_case_ids.add(case_id)
            binary = Path(raw_path)
            if not binary.is_absolute():
                binary = (ROOT / binary).resolve()
            else:
                binary = binary.resolve()
            if binary.suffix.lower() != ".prx" or not binary.is_file():
                parser.error(f"campaign PRX is missing or does not end in .prx: {raw_path}")
            try:
                binary.relative_to(host0_root)
            except ValueError:
                parser.error("each campaign PRX must be inside --host0-root")
            cases.append(CampaignCase(case_id, binary, args.timeout))
        if args.out:
            campaign_output = args.out.resolve()
            try:
                campaign_output.relative_to(host0_root)
            except ValueError:
                parser.error("campaign --out must be inside --host0-root scratch")
            if campaign_output == host0_root / "nakagawa_transport_write.bin":
                parser.error("campaign --out cannot replace the transport-write evidence file")
            if campaign_output in {case.binary for case in cases}:
                parser.error("campaign --out cannot replace a probe PRX")
        try:
            pspsh_argv = _argv_json(parser, args.pspsh_argv_json, "--pspsh-argv-json")
            usbhostfs_argv = _argv_json(
                parser, args.usbhostfs_argv_json, "--usbhostfs-argv-json"
            )
        except SystemExit:
            raise
        transport = PsplinkProcessTransport(
            pspsh_argv=pspsh_argv,
            usbhostfs_argv=usbhostfs_argv,
            host0_root=host0_root,
            session_id=args.session_id,
        )
        runner = PsplinkCampaignRunner(
            transport,
            console_model=args.model,
            source_commit=args.source_commit,
            model_code=args.model_code,
            expected_firmware=args.firmware,
            shell_verification_timeout=args.shell_verification_timeout,
        )
        report = runner.run(cases)
        rendered = dump_json(report)
        if args.out:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(rendered, encoding="utf-8")
        else:
            sys.stdout.write(rendered)
        if report["terminal_reason"] == "PHYSICAL_INTERVENTION_REQUIRED":
            return 3
        if report["terminal_reason"]:
            return 2
        return 0 if all(item["ACCEPTANCE_ELIGIBLE"] for item in report["envelopes"]) else 2

    supplied = [
        flag
        for flag in PROVENANCE_FLAGS
        if getattr(args, flag)
    ]
    required_provenance_count = len(PROVENANCE_FLAGS)
    if supplied and len(supplied) != required_provenance_count:
        missing_flags = [
            flag for flag in PROVENANCE_FLAGS
            if flag not in supplied
        ]
        missing = ", ".join("--" + flag.replace("_", "-") for flag in missing_flags)
        parser.error(f"provenance metadata is all-or-nothing; missing {missing}")

    if bool(args.host0_output) != args.validate_dmac_size_matrix:
        parser.error("--host0-output and --validate-dmac-size-matrix must be supplied together")

    if bool(args.annotate_report) != bool(args.observed_terminal_outcome):
        parser.error("--annotate-report and --observed-terminal-outcome must be supplied together")
    if args.annotate_report:
        if args.command or args.psp_output or args.nakagawa_output or args.host0_output or args.dry_run:
            parser.error("terminal annotation cannot launch or compare another capture")
        report = json.loads(args.annotate_report.read_text(encoding="utf-8"))
        stdout_file = report.get("stdout_file")
        if not isinstance(stdout_file, str):
            parser.error("capture report has no stdout_file")
        capture = (ROOT / stdout_file).resolve()
        try:
            capture.relative_to(args.results_directory)
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
    if not args.session_id or not _SESSION_ID_RE.fullmatch(args.session_id):
        parser.error("--command touches the PSP and requires --session-id naming the hardware lock holder")
    try:
        require_hardware_lock(args.session_id)
    except HardwareLockError as exc:
        refusal = dump_json({
            "schema": 1, "mode": "capture", "status": "REFUSED", "reason": exc.status,
        })
        if args.out:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(refusal, encoding="utf-8")
        else:
            sys.stdout.write(refusal)
        return 2
    capture_started_ns = time.time_ns()
    returncode, stdout, stderr, process_status = _run_command(command, args.timeout)
    args.results_directory.mkdir(parents=True, exist_ok=True)
    timestamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    capture = args.results_directory / f"psplink-{timestamp}.stdout.txt"
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
    capture_ok = True
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
        capture_ok = False
    if args.host0_output:
        try:
            host0_text = _wait_for_host0_output(
                args.host0_output,
                args.timeout,
                not_before_ns=capture_started_ns,
                ready=lambda text: _host0_capture_complete(text, args),
            )
            host0_capture = args.results_directory / f"psplink-{timestamp}.host0.txt"
            host0_capture.write_text(host0_text, encoding="utf-8")
            report["host0_file"] = str(host0_capture.relative_to(ROOT)).replace("\\", "/")
            try:
                report["host0_validation"] = _validate_host0_capture(host0_text, args)
            except (ProtocolError, OSError, ValueError) as exc:
                report["host0_validation"] = {
                    "classification": "INCONCLUSIVE",
                    "acceptance_eligible": False,
                    "acceptance_blockers": [str(exc)],
                }
                capture_ok = False
            if report["host0_validation"]["classification"] != "PASS":
                capture_ok = False
        except (TimeoutError, OSError) as exc:
            report["host0_validation"] = {
                "classification": "INCONCLUSIVE",
                "acceptance_eligible": False,
                "acceptance_blockers": [str(exc)],
            }
            capture_ok = False
    rendered = dump_json(report)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(rendered, encoding="utf-8")
    else:
        sys.stdout.write(rendered)
    return 0 if process_status == "PROCESS_EXITED" and returncode == 0 and capture_ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
