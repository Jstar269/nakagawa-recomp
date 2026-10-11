# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Executable conformance suite for the hardware runner autonomy design.

This module is simultaneously:

* a reference implementation of the framed command protocol, the host
  orchestrator state machine, the bounded recovery ladder, and the evidence
  envelope described in docs/HARDWARE_RUNNER_AUTONOMY.md, and
* a deterministic simulation suite that drives both a cooperative runner
  model and a fault-injecting fake transport through every failure mode the
  autonomy program must survive.

Everything here is synthetic: no PSP, no usbhostfs, no private bytes. The
contract for future real implementations is to import this module's
Orchestrator/protocol pieces (or satisfy them behaviourally) so hosted CI
proves the control logic before silicon ever sees it.

Fail-closed invariants under test, in one sentence each:

* a transport fault must never become a PSP semantic result;
* unqualified epochs must not collect results;
* stale, mismatched-identity, or partial evidence must be rejected, not averaged in;
* recovery is a bounded ladder whose exhaustion is an explicit
  PHYSICAL_INTERVENTION_REQUIRED stop, never an infinite retry loop;
* raw device values and operator-declared labels are preserved separately from
  versioned interpretations and computed agreement.
"""

from __future__ import annotations

import contextlib
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import struct
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))

from psp_oracle.run_psplink import (
    CAMPAIGN_QUEUE_CASES,
    CampaignCase,
    PsplinkCampaignRunner,
    _campaign_host0_log_path,
    _parse_campaign_records,
    _FIXED_CAMPAIGN_CASES,
    _campaign_queue_summary,
    _checkpoint_after_run,
    _checkpoint_case_finished,
    _checkpoint_case_started,
    _checkpoint_resume,
    _parse_usbipd_psplink_devices,
    _snapshot_host0_output,
    _wait_for_host0_output,
    _verify_psplink_shell,
    HardwareLockError,
    PsplinkProcessTransport,
    TransportStartError,
    UnsafeHost0OutputError,
    _run_command,
    run_campaign_plan,
    parse_psplink_meminfo,
    parse_psplink_module_list,
    parse_psplink_module_threads,
    parse_psplink_thread_snapshot,
)
from psp_oracle import run_psplink as run_psplink_module
from psp_oracle.parse_golden import (
    DMAC_INVALID_CASES,
    CACHE_SPEC,
    EXPECTED_CELLS as FPU_EXPECTED_CELLS,
    IO_SPEC,
    MBX_DELETE_WAIT_EXPECTED_FIELDS,
    MBX_DELETE_WAIT_SPEC,
)
from psp_oracle.protocol import ParsedOutput, TestResult
from psp_oracle.protocol import ProtocolError as PspProtocolError
from psp_oracle.protocol import model_identity_fields

MAGIC = b"NR"
VERSION = 2

HELLO = 0x01
CAPABILITIES = 0x02
META = 0x03
LOAD_CASE = 0x04
RUN_CASE = 0x05
RESULT = 0x06
RESET_CASE = 0x07
PING = 0x08
STOP = 0x09

STATUS_OK = "OK"
STATUS_ERROR = "ERROR"

FAULT_NONE = "none"
FAULT_TRANSPORT = "transport"
FAULT_IDENTITY = "identity"
FAULT_SEMANTIC = "semantic"
FAULT_EVIDENCE = "evidence"


class ProtocolError(ValueError):
    """A frame violates the wire format."""


class TransportDead(Exception):
    """No bytes will ever arrive again until recovery reopens the link."""


class CommandTimeout(Exception):
    """The peer produced no complete frame inside its deadline."""


def frame_encode(msg_type: int, seq: int, payload: bytes) -> bytes:
    if not 0 <= seq <= 0xFFFF:
        raise ProtocolError("seq out of range")
    header = MAGIC + struct.pack(
        "<BBHII", VERSION, msg_type, seq, len(payload), (~len(payload)) & 0xFFFFFFFF
    )
    return header + payload


def frame_decode(buf: memoryview):
    """Return (type, seq, payload, consumed) or None if more bytes are needed."""
    if len(buf) < 14:
        return None
    if bytes(buf[:2]) != MAGIC:
        raise ProtocolError("bad magic")
    version, msg_type, seq, length, complement = struct.unpack("<BBHII", buf[2:14])
    if version != VERSION:
        raise ProtocolError("unsupported protocol version")
    if (complement ^ 0xFFFFFFFF) != length:
        raise ProtocolError("length check failed")
    total = 14 + length
    if len(buf) < total:
        return None
    return msg_type, seq, bytes(buf[14:total]), total


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


RUNNER_SHA = _sha(b"resident-oracle-runner v1 synthetic")
SOURCE_COMMIT = "c75c303a1e6885eb6f8bb6875afde527d72ab688"
EPOCH = "epoch-0001"

CAMPAIGN_META = (
    "NAKAGAWA_PSP_META schema=1 source=psp model=unknown firmware=unknown "
    "binary_sha256=" + "0" * 64 + " source_commit=" + "0" * 40 + " fixture=test\n"
)


def _campaign_fpu_line(case_id: str) -> str:
    if case_id == "fpu-boot-fcr31":
        return (
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FPU-001 "
            f"case_id={case_id} status=PASS result=0x00000e00 "
            "out0=0x00000e00 out1=0x0000001c out2=0x00000000\n"
        )
    if case_id == "fpu-ftz-contrast":
        return (
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FPU-001 "
            f"case_id={case_id} status=PASS result=0x00000000 "
            "out0=0x00000000 out1=0x00000000 out2=0x00000000 out3=0x01000000\n"
        )
    if case_id == "fpu-done":
        return (
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FPU-001 "
            f"case_id={case_id} status=PASS result=0x00000000 out0=0x0000000f\n"
        )
    return (
        "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-FPU-001 "
        f"case_id={case_id} status=PASS result=0x00000000 out0=0x00000000\n"
    )


def _campaign_spec_line(spec, case_id: str) -> str:
    if case_id == spec.terminal_case:
        out0 = spec.terminal_count
    else:
        out0 = 1
    suffix = f"out0=0x{out0:08x}"
    if case_id != spec.terminal_case:
        suffix += " out1=0x00000002"
    return (
        f"NAKAGAWA_PSP_TEST schema=1 test_id={spec.test_id} case_id={case_id} "
        f"status=PASS result=0x00000000 {suffix}\n"
    )


def _campaign_fpu_stream(cells=FPU_EXPECTED_CELLS) -> str:
    return CAMPAIGN_META + "".join(_campaign_fpu_line(cell) for cell in cells)


def _campaign_spec_stream(spec, cases=None) -> str:
    selected = spec.ordered_cases if cases is None else cases
    return CAMPAIGN_META + "".join(_campaign_spec_line(spec, case) for case in selected)


def _campaign_mbx_delete_wait_line(case_id: str) -> str:
    test_id = MBX_DELETE_WAIT_SPEC.test_id
    fields = MBX_DELETE_WAIT_EXPECTED_FIELDS[case_id]
    line = (
        f"NAKAGAWA_PSP_TEST schema=1 test_id={test_id} case_id={case_id} "
        "status=PASS result=0x00000000"
    )
    if case_id == MBX_DELETE_WAIT_SPEC.terminal_case:
        return line + f" out0=0x{MBX_DELETE_WAIT_SPEC.terminal_count:08x}\n"
    count = len(fields) - 1
    return line + " " + " ".join(
        f"out{index}=0x{index + 1:08x}" for index in range(count)
    ) + "\n"


def _campaign_mbx_delete_wait_stream(cases=None) -> str:
    selected = MBX_DELETE_WAIT_SPEC.ordered_cases if cases is None else cases
    return CAMPAIGN_META + "".join(
        _campaign_mbx_delete_wait_line(case_id) for case_id in selected
    )


class LiveServerOutput:
    """Blocking line stream for a fake USBHostFS: lines arrive when emitted."""

    def __init__(self, lines=()):
        self._lines: queue.Queue[str | None] = queue.Queue()
        for line in lines:
            self.emit(line)

    def emit(self, line: str) -> None:
        self._lines.put(line + "\n")

    def close(self) -> None:
        self._lines.put(None)

    def __iter__(self):
        return self

    def __next__(self) -> str:
        line = self._lines.get(timeout=10.0)
        if line is None:
            raise StopIteration
        return line


class FakeUsbHostFsProcess:
    """A synthetic usbhostfs_pc process whose output the test controls."""

    def __init__(self, lines=(), *, exits: bool = False):
        self.stdout = LiveServerOutput(lines)
        self.returncode = None
        self.terminated = False
        if exits:
            self.stdout.close()

    def poll(self):
        return self.returncode

    def terminate(self):
        self.terminated = True
        self.returncode = 0
        self.stdout.close()

    def wait(self, timeout):
        return self.returncode

    def kill(self):
        self.terminate()


USBHOSTFS_BANNER = ("USBHostFS (c) TyRaNiD 2k6", "waiting for device...")


class SimulatedPsplinkTransport:
    """Command-level fake for the real PSPSH adapter; never touches hardware."""

    def __init__(
        self,
        *,
        timeout_cases: set[str] | None = None,
        fail_modstun: bool = False,
        fail_modstun_cases: set[str] | None = None,
        fail_post_case_ver_once: bool = False,
        fail_first_ver_once: bool = False,
        fail_all_ver: bool = False,
        unknown_command_ver_once: bool = False,
        reset_timeout: bool = False,
        reset_returncode: int = 0,
        stdout_record_cases: set[str] | None = None,
        stdout_result_overrides: dict[str, str] | None = None,
        host0_log_contents: dict[str, str] | None = None,
        model_profile_raw_value: str = "0x3",
        device_source_commit: str | None = SOURCE_COMMIT,
        write_host0_logs: bool = True,
        stale_host0_mtime: bool = False,
        fail_host0_roundtrip_cases: set[str] | None = None,
        transport_file_cases: set[str] | None = None,
        fail_snapshot_call: dict[str, int] | None = None,
        start_error: str | None = None,
        lock_held_checks: int | None = None,
        lock_refusal_status: str = "HARDWARE_LOCK_NOT_HELD",
        unfinished_cases: set[str] | None = None,
    ):
        self.unfinished_cases = unfinished_cases or set()
        self.start_error = start_error
        self.lock_held_checks = lock_held_checks
        self.lock_refusal_status = lock_refusal_status
        self.lock_checks = 0
        self.timeout_cases = timeout_cases or set()
        self.fail_modstun = fail_modstun
        self.fail_modstun_cases = fail_modstun_cases or set()
        self.fail_post_case_ver_once = fail_post_case_ver_once
        self.fail_first_ver_once = fail_first_ver_once
        self.fail_all_ver = fail_all_ver
        self.unknown_command_ver_once = unknown_command_ver_once
        self.first_ver_failed = False
        self.reset_timeout = reset_timeout
        self.reset_returncode = reset_returncode
        self.stdout_record_cases = stdout_record_cases or set()
        self.stdout_result_overrides = stdout_result_overrides or {}
        self.host0_log_contents = host0_log_contents or {}
        self.model_profile_raw_value = model_profile_raw_value
        self.device_source_commit = device_source_commit
        self.write_host0_logs = write_host0_logs
        self.stale_host0_mtime = stale_host0_mtime
        self.fail_host0_roundtrip_cases = fail_host0_roundtrip_cases or set()
        self.transport_file_cases = transport_file_cases
        self.fail_snapshot_call = dict(fail_snapshot_call or {})
        self.snapshot_command_calls: dict[str, int] = {}
        self.post_case_ver_failed = False
        self.case_started = False
        self.started = False
        self.stopped = False
        self.restarts = 0
        self.commands: list[tuple[str, float]] = []
        self.host0_root: Path | None = None
        self.stale_log_present_at_load = False
        self.host0_record_counts_at_unload: dict[str, int] = {}
        self.current_case = ""
        self.waiting_for_device = False
        self.transport_recoveries = 0
        self.unknown_command_events = 0
        self._baseline_threads = frozenset({
            ("0x00000001", "PspLink"),
            ("0x00000002", "USBThread"),
        })
        self._baseline_modules = frozenset({("0x00000003", "PspLink")})
        self._probe_uid = "0x04280001"
        self._probe_thread = ("0x04280002", "user_main")
        self._probe_loaded = False
        self._baseline_memory = {
            1: ("0x08800000", 33554432, 20971520, 16777216),
            2: ("0x88000000", 53687091, 33554432, 25165824),
        }

    def _snapshot_threads(self) -> str:
        threads = set(self._baseline_threads)
        if self._probe_loaded:
            threads.add(self._probe_thread)
        rows = "".join(
            f"UID: {uid} - Name: {name}\n"
            for uid, name in sorted(threads)
        )
        return f"<Thread List ({len(threads)} entries)>\n" + rows

    def _snapshot_memory(self) -> str:
        rows = []
        for partition, (base, size, total_free, max_free) in sorted(
            self._baseline_memory.items()
        ):
            if self._probe_loaded and partition == 1:
                # Synthetic fixture allocation: S1 differs from S0 and returns to
                # baseline only after unload/reset. This is not PSP memory data.
                total_free -= 4096
                max_free -= 4096
            rows.append(
                f"{partition}  | {base} | {size:8d} | {total_free:9d} | "
                f"{max_free:9d} | 000F |\n"
            )
        return (
            "Memory Partitions:\n"
            "N  |    BASE    |   SIZE   | TOTALFREE |  MAXFREE  | ATTR |\n"
            "---|------------|----------|-----------|-----------|------|\n"
            + "".join(rows)
        )

    def _snapshot_modules(self) -> str:
        modules = set(self._baseline_modules)
        if self._probe_loaded:
            modules.add((self._probe_uid, "NAKAGAWA_PSP_ORACLE"))
        rows = "".join(
            f"UID: {uid} Attr: 0000 - Name: {name}\n"
            for uid, name in sorted(modules)
        )
        return f"<Module List ({len(modules)} modules)>\n" + rows

    def _module_info(self) -> str:
        if not self._probe_loaded:
            return f"ERROR: Unknown module {self._probe_uid}\n"
        return (
            f"UID: {self._probe_uid} Attr: 0000 - Name: NAKAGAWA_PSP_ORACLE\n"
            "Module Thread (1)\n"
            f"UID: {self._probe_thread[0]} - Name: {self._probe_thread[1]}\n"
        )

    def check_hardware_lock(self) -> None:
        self.lock_checks += 1
        if self.lock_held_checks is not None and self.lock_checks > self.lock_held_checks:
            raise HardwareLockError(self.lock_refusal_status)

    def start(self) -> None:
        self.check_hardware_lock()
        if self.start_error is not None:
            raise TransportStartError(self.start_error)
        self.started = True

    def stop(self) -> None:
        self.stopped = True

    def restart(self) -> None:
        self.restarts += 1
        self.started = True

    def take_waiting_for_device(self) -> bool:
        waiting, self.waiting_for_device = self.waiting_for_device, False
        return waiting

    def take_unknown_command_events(self) -> int:
        events, self.unknown_command_events = self.unknown_command_events, 0
        return events

    def recover_psplink_transport(
        self,
        timeout: float,
        *,
        shell_verification_timeout: float = 45.0,
        record_event=None,
    ):
        self.transport_recoveries += 1
        if self.reset_timeout:
            return False, "simulated re-attach failure", None
        verified, result, _attempts, detail = _verify_psplink_shell(
            self.run,
            shell_verification_timeout,
            take_unknown_command_events=self.take_unknown_command_events,
            record_event=record_event,
        )
        return verified, f"simulated PSPLink re-attach: {detail}", result

    def run(self, command: str, timeout: float):
        self.commands.append((command, timeout))
        if command in {"thlist", "meminfo", "modlist"}:
            call_number = self.snapshot_command_calls.get(command, 0) + 1
            self.snapshot_command_calls[command] = call_number
            if call_number == self.fail_snapshot_call.get(command):
                return None, "", "", "TIMEOUT"
        if command == "thlist":
            return 0, self._snapshot_threads(), "", "PROCESS_EXITED"
        if command == "meminfo":
            return 0, self._snapshot_memory(), "", "PROCESS_EXITED"
        if command == "modlist":
            return 0, self._snapshot_modules(), "", "PROCESS_EXITED"
        if command == "ver" and self.case_started and self.fail_post_case_ver_once and not self.post_case_ver_failed:
            self.post_case_ver_failed = True
            return None, "", "", "TIMEOUT"
        if command == "ver":
            if self.fail_all_ver:
                return None, "", "", "TIMEOUT"
            if self.fail_first_ver_once and not self.first_ver_failed:
                self.first_ver_failed = True
                return None, "", "", "TIMEOUT"
            if self.unknown_command_ver_once:
                self.unknown_command_ver_once = False
                self.unknown_command_events += 1
                return 0, "Error, unknown command 00000000\n", "", "PROCESS_EXITED"
            return 0, "PSPLink v3.2.1\n", "", "PROCESS_EXITED"
        if command == "usbstat":
            return 0, "USB Connection: established\n", "", "PROCESS_EXITED"
        if command == "pwd":
            return 0, "host0:/\n", "", "PROCESS_EXITED"
        if command == "pspver":
            return 0, "Version: 6.6.1 (0x06060110)\n", "", "PROCESS_EXITED"
        if command == "exprint":
            return 0, "Synthetic exception query is unqualified\n", "", "PROCESS_EXITED"
        if command.startswith("ldstart host0:/"):
            case_id = Path(command).name.removesuffix(".prx")
            self.current_case = case_id
            self.case_started = True
            self._probe_loaded = True
            if (
                self.host0_root is not None
                and case_id not in self.fail_host0_roundtrip_cases
                and (self.transport_file_cases is None or case_id in self.transport_file_cases)
            ):
                pattern = bytes(
                    (0x5A ^ (index * 0x25 + (index >> 3))) & 0xFF
                    for index in range(64)
                )
                (self.host0_root / "nakagawa_transport_write.bin").write_bytes(pattern)
            if case_id in self.timeout_cases:
                return None, f"Load/Start UID: {self._probe_uid}\nNAKAGAWA_PSP_TEST partial", "", "TIMEOUT"
            log_stem = case_id.replace("-", "_")
            if log_stem.startswith("dma_"):
                log_stem = "dmac_" + log_stem[4:]
            host0_log = self.host0_root / f"{log_stem}_log.txt" if self.host0_root else None
            self.stale_log_present_at_load = bool(host0_log and host0_log.exists())
            metadata_record = (
                "NAKAGAWA_PSP_META schema=1 source=psp model=fixture firmware=test "
                "binary_sha256=" + "0" * 64
                + (
                    " source_commit=" + self.device_source_commit
                    if self.device_source_commit is not None
                    else ""
                )
                + "\n"
            )
            if case_id == "transport-write":
                result_record = (
                    "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-TRANSPORT-001 "
                    "case_id=host0-write-readback status=PASS result=0x0\n"
                )
            elif case_id == "model-profile":
                result_record = (
                    "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-SYSTEM-001 "
                    f"case_id=model-profile status=PASS result={self.model_profile_raw_value} "
                    f"out0={self.model_profile_raw_value} out1=0x06060110 out2=0xde\n"
                )
            else:
                result_record = (
                    "NAKAGAWA_PSP_TEST schema=1 test_id=SYNTHETIC case_id=" + case_id
                    + " status=PASS result=0x1\n"
                )
            complete_host0_log = metadata_record + result_record + (
                "" if case_id in self.unfinished_cases
                else "NAKAGAWA_PSP_COMPLETE schema=1 status=PASS\n"
            )
            stdout_record = result_record
            if case_id in self.stdout_result_overrides and case_id != "transport-write":
                stdout_record = (
                    "NAKAGAWA_PSP_TEST schema=1 test_id=SYNTHETIC case_id=" + case_id
                    + " status=PASS result=" + self.stdout_result_overrides[case_id] + "\n"
                )
            if self.write_host0_logs and self.host0_root is not None:
                assert host0_log is not None
                host0_log.write_bytes(
                    self.host0_log_contents.get(case_id, complete_host0_log).encode("utf-8")
                )
                if self.stale_host0_mtime:
                    os.utime(host0_log, ns=(1, 1))
            stdout_records = "" if case_id in self.stdout_record_cases else metadata_record + stdout_record
            return 0, (
                f"Load/Start host0:/{case_id}.prx UID: {self._probe_uid}\n"
                + stdout_records
            ), "", "PROCESS_EXITED"
        if command == "modstun 0x04280001":
            if self.host0_root is not None and self.current_case:
                host0_log = _campaign_host0_log_path(self.host0_root, self.current_case)
                if host0_log.is_file():
                    self.host0_record_counts_at_unload[self.current_case] = sum(
                        line.startswith("NAKAGAWA_PSP_TEST ")
                        for line in host0_log.read_text(encoding="utf-8").splitlines()
                    )
            if (
                (self.fail_modstun and self.current_case != "transport-write")
                or self.current_case in self.fail_modstun_cases
            ):
                return 1, "Module Stop/Unload failed\n", "", "PROCESS_EXITED"
            if not self._probe_loaded:
                return 1, f"ERROR: Unknown module {self._probe_uid}\n", "", "PROCESS_EXITED"
            self._probe_loaded = False
            return 0, f"Module Stop/Unload 0x00000000/{self._probe_uid} Status 0xDEADBEEF\n", "", "PROCESS_EXITED"
        if command == f"modinfo {self._probe_uid} t":
            if not self._probe_loaded:
                return 1, self._module_info(), "", "PROCESS_EXITED"
            return 0, self._module_info(), "", "PROCESS_EXITED"
        if command == f"modinfo {self._probe_uid}":
            if self._probe_loaded:
                return 0, self._module_info(), "", "PROCESS_EXITED"
            return 1, self._module_info(), "", "PROCESS_EXITED"
        if command == "reset" and self.reset_timeout:
            return None, "", "", "TIMEOUT"
        if command == "reset":
            self._probe_loaded = False
            return self.reset_returncode, "Reset\n", "", "PROCESS_EXITED"
        raise AssertionError(f"unexpected PSPLINK command: {command}")


class RunnerModel:
    """Simulated device side: cooperative, configurable to misbehave."""

    def __init__(self, **behavior):
        self.behavior = {
            "wrong_runner_sha": False,
            "wrong_source_commit": False,
            "crash_on_case": None,
            "duplicate_result": False,
            "partial_result": False,
            "raw_model_value": "3",
            "physical_model_label": "psp-3000-series",
        }
        self.behavior.update(behavior)
        self.loaded = None
        self.alive = True
        self.results_sent = 0

    def respond(self, msg_type: int, payload: bytes) -> bytes | None:
        b = self.behavior
        if msg_type == LOAD_CASE:
            # Payload shape: case=<id>\n...
            for line in payload.split(b"\n"):
                if line.startswith(b"case="):
                    self.loaded = line[5:].decode()
                    break
            return frame_encode(msg_type, 1, b"status=OK\nloaded=" +
                                (self.loaded or "").encode() + b"\n")
        if msg_type == HELLO:
            sha = ("dead" * 16) if b["wrong_runner_sha"] else RUNNER_SHA
            commit = ("f" * 64) if b["wrong_source_commit"] else SOURCE_COMMIT
            body = f"runner_sha={sha}\nsource_commit={commit}".encode()
        elif msg_type == META:
            body = (
                f"raw_model={b['raw_model_value']}\n"
                f"label={b['physical_model_label']}\nfw=6.61\ncfw=ark\n"
            ).encode()
        elif msg_type == RUN_CASE:
            if b["crash_on_case"] is not None and b["crash_on_case"] == self.loaded:
                self.alive = False
                return None
            case = self.loaded
            result = f"NAKAGAWA_PSP_TEST test={case} case={case} iteration=1 status=PASS".encode()
            # The declared length/hash always describe the FULL result; the
            # partial_result fault truncates only what goes on the wire, which
            # is exactly how a partial delivery manifests.
            body = (
                f"status={STATUS_OK}\nresult_len={len(result)}\n"
                f"result_sha256={_sha(result)}\n\nglobal_epoch={EPOCH}\n"
            ).encode() + (result[: len(result) // 2] if b["partial_result"] else result)
            self.results_sent += 1
            if b["duplicate_result"]:
                # A second RESULT frame for the same seq is injected by the
                # transport layer below, not by this method.
                pass
        elif msg_type in (PING, CAPABILITIES, RESET_CASE):
            body = b"status=OK\n"
        elif msg_type == STOP:
            body = b"status=OK\nbye=1\n"
        else:
            body = b"status=OK\n"
        return frame_encode(msg_type, 1, body)


class FakeTransport:
    """Deterministic byte pipe with fault injection points."""

    def __init__(self, runner: RunnerModel):
        self.runner = runner
        self.rx = bytearray()
        self.connected = False
        self.usb_server_hangs = False
        self.echo_without_frame = False
        self.drop_after_send = False
        self.dead = False
        self.inject_after_next_request = None
        self.stale_first = False

    def connect(self) -> None:
        if self.usb_server_hangs:
            raise CommandTimeout("usbhostfs never attaches")
        self.connected = True

    def send(self, data: bytes) -> None:
        if self.dead or not self.runner.alive:
            raise TransportDead("link is down")
        if self.echo_without_frame:
            self.rx += b"psp> " + data.split(b"\n")[0] + b"\n"
            return
        resp = self.runner.respond(data[0], data[1:])
        if resp is None:
            self.dead = True
            raise TransportDead("runner stopped responding mid-request")
        if self.inject_after_next_request is not None and self.stale_first:
            self.rx += self.inject_after_next_request + resp
            self.inject_after_next_request = None
        else:
            self.rx += resp
            if self.inject_after_next_request is not None:
                self.rx += self.inject_after_next_request
                self.inject_after_next_request = None
        if self.drop_after_send:
            self.dead = True

    def poll(self) -> bytes:
        if self.dead or not self.runner.alive:
            raise TransportDead("link is down")
        out = bytes(self.rx)
        self.rx.clear()
        return out


class Orchestrator:
    """Reference control plane. Collects envelopes only while qualified."""

    def __init__(self, transport: FakeTransport, max_l1_recoveries: int = 1):
        self.transport = transport
        self.state = "OFFLINE"
        self.envelopes: list[dict] = []
        self.recovery_events: list[str] = []
        self.l1_used = 0
        self.max_l1_recoveries = max_l1_recoveries
        self.terminal_reason = None
        self.expected_identity = {"runner_sha": RUNNER_SHA, "source_commit": SOURCE_COMMIT}
        self.qualified = False

    # -- helpers -------------------------------------------------------
    def _request(self, msg_type: int, extra: bytes = b""):
        payload = bytes([msg_type]) + extra + b"\nglobal_epoch=" + EPOCH.encode()
        self.transport.send(payload)
        buf = bytearray()
        pos = 0
        while True:
            chunk = self.transport.poll()  # raises TransportDead / returns bytes
            buf += chunk
            while True:
                try:
                    decoded = frame_decode(memoryview(buf)[pos:])
                except ProtocolError:
                    # Banner/echo pollution: resync by scanning forward one
                    # byte for the next magic; if the pipe is exhausted first,
                    # the timeout below fires.
                    pos += 1
                    if pos >= len(buf):
                        break
                    continue
                if decoded is None:
                    break
                got_type, _seq, body, used = decoded
                pos += used
                if b"global_epoch=" in body:
                    epoch = body.split(b"global_epoch=")[1].splitlines()[0]
                    if epoch.decode(errors="replace") != EPOCH:
                        continue  # stale/foreign-epoch traffic: discard, keep waiting
                if got_type != msg_type:
                    raise ProtocolError(f"reply type {got_type} != request {msg_type}")
                return body
            if not chunk:
                raise CommandTimeout("peer went quiet without a full frame")

    def _escalate(self, level: str, detail: str) -> None:
        self.recovery_events.append(f"{level}: {detail}")
        if level == "L4":
            self.state = "SESSION_WEDGED"
            self.terminal_reason = "PHYSICAL_INTERVENTION_REQUIRED"

    # -- protocol phases ----------------------------------------------
    def qualify(self) -> None:
        """WAIT_DEVICE -> ... -> READY with behavioral qualification only."""
        self.transport.connect()
        hello = self._request(HELLO).decode()
        fields = dict(line.split("=", 1) for line in hello.strip().splitlines())
        if fields.get("runner_sha") != self.expected_identity["runner_sha"]:
            self._escalate("L0", "runner sha mismatch -> abort epoch")
            self.terminal_reason = "IDENTITY_MISMATCH"
            self.state = "STOPPED"
            return
        if fields.get("source_commit") != self.expected_identity["source_commit"]:
            self._escalate("L0", "source commit mismatch -> abort epoch")
            self.terminal_reason = "IDENTITY_MISMATCH"
            self.state = "STOPPED"
            return
        meta = self._request(META).decode()
        mfields = dict(line.split("=", 1) for line in meta.strip().splitlines())
        self.meta = mfields
        ping = self._request(PING)
        if b"status=OK" not in ping:
            raise ProtocolError("PING answered without OK")
        self.qualified = True
        self.state = "READY"

    def run_case(self, case_id: str, iteration: int = 1) -> None:
        assert self.qualified, "cases may only run in a qualified epoch"
        self.state = "RUN_CASE"
        try:
            self._request(LOAD_CASE, f"case={case_id}\n".encode())
            body = self._request(RUN_CASE)
        except (TransportDead, CommandTimeout, ProtocolError) as error:
            self._on_transport_fault(case_id, error)
            return
        text = body.decode(errors="replace")
        head, sep, result_lines = text.partition("\n\n")
        if not sep:
            self._on_transport_fault(case_id, CommandTimeout("frame ended mid-result"))
            return
        fields = dict(line.split("=", 1) for line in head.strip().splitlines())
        declared_len = int(fields["result_len"])
        result = (
            result_lines[len(result_lines) - declared_len:].encode()
            if declared_len else b""
        )
        if len(result) != declared_len or _sha(result) != fields["result_sha256"]:
            self._escalate("L0", "partial/corrupt result rejected")
            self.terminal_reason = "EVIDENCE_REJECTED"
            self.state = "RECOVERABLE_FAULT"
            return
        self.envelopes.append(self._envelope(case_id, iteration, result.decode(), fields))
        self.state = "VERIFY_RESULT"

    def _envelope(self, case_id, iteration, raw_result, fields) -> dict:
        return {
            "CONSOLE_ID": "sim-unit-001",
            **model_identity_fields(
                self.meta.get("label"), self.meta.get("raw_model")
            ),
            "SOURCE_COMMIT": SOURCE_COMMIT,
            "BINARY_SHA256": RUNNER_SHA,
            "RUNNER_SHA256": RUNNER_SHA,
            "CASE_ID": case_id,
            "ITERATION": iteration,
            "RAW_RESULT": raw_result,
            "GLOBAL_EPOCH": EPOCH,
            "RECOVERY_EVENTS": list(self.recovery_events),
            "QUALIFICATION_STATUS": "QUALIFIED",
            "EVIDENCE_CLASS": "PRODUCTION_HELPER",
            "WHAT_IS_NOT_PROVEN": "simulation only; no silicon observation occurred",
        }

    def _on_transport_fault(self, case_id: str, error: Exception) -> None:
        """Bounded ladder: L0 once, L1 once, then L4. Never fabricates."""
        self.state = "RECOVERABLE_FAULT"
        self._escalate("L0", f"transport fault during {case_id}: {error!r}")
        if isinstance(error, TransportDead) and not self.transport.runner.alive:
            self._escalate("L3", "control process dead; standalone relaunch required")
        if self.terminal_reason is None and self.l1_used < self.max_l1_recoveries:
            self.l1_used += 1
            self._escalate("L1", "host stack restart and requalify")
            self.terminal_reason = "RECOVERED_WITH_L1"
            self.state = "OFFLINE"
            return
        if self.terminal_reason is None:
            self._escalate("L4", "recovery budget exhausted")
            self.terminal_reason = "PHYSICAL_INTERVENTION_REQUIRED"
            self.state = "SESSION_WEDGED"


def _lock_held(_session_id):
    return True, "HELD_AND_CONFIRMED"


class HardwareRunnerProtocolTests(unittest.TestCase):
    def setUp(self):
        source_check = patch("psp_oracle.run_psplink._check_source_tree", return_value=None)
        source_check.start()
        self.addCleanup(source_check.stop)

    def build(self, **behavior) -> tuple[Orchestrator, RunnerModel, FakeTransport]:
        runner = RunnerModel(**behavior)
        transport = FakeTransport(runner)
        orch = Orchestrator(transport)
        return orch, runner, transport

    # -- 1. successful attach and qualified case ----------------------
    def test_01_successful_attach_runs_case_and_writes_envelope(self):
        orch, _, _ = self.build()
        orch.qualify()
        self.assertEqual(orch.state, "READY")
        self.assertTrue(orch.qualified)
        orch.run_case("vfpu_a_01")
        self.assertEqual(orch.state, "VERIFY_RESULT")
        self.assertEqual(len(orch.envelopes), 1)
        env = orch.envelopes[0]
        self.assertIn("status=PASS", env["RAW_RESULT"])
        self.assertEqual(env["QUALIFICATION_STATUS"], "QUALIFIED")

    # -- 14. raw model and physical label remain separately auditable -----
    def test_14_model_identity_uses_the_versioned_pspsdk_rule(self):
        orch, _, _ = self.build()
        orch.qualify()
        orch.run_case("meta_probe")
        env = orch.envelopes[-1]
        self.assertEqual(env["SOFTWARE_MODEL_RAW_VALUE"], "3")
        self.assertEqual(env["PHYSICAL_MODEL_LABEL"], "psp-3000-series")
        self.assertEqual(env["INTERPRETED_MODEL_FAMILY"], "PSP-3000")
        self.assertEqual(env["MODEL_INTERPRETATION_RULE"], "PSPSDK_PMODEL_ORDINAL_V1")
        self.assertEqual(env["MODEL_IDENTITY_AGREEMENT"], "AGREES")

    # -- 12/13. identity binding fails closed --------------------------
    def test_12_wrong_binary_sha_aborts_before_cases(self):
        orch, _, _ = self.build(wrong_runner_sha=True)
        orch.qualify()
        self.assertFalse(orch.qualified)
        self.assertEqual(orch.terminal_reason, "IDENTITY_MISMATCH")
        self.assertEqual(orch.envelopes, [])

    def test_13_wrong_source_commit_aborts_before_cases(self):
        orch, _, _ = self.build(wrong_source_commit=True)
        orch.qualify()
        self.assertEqual(orch.terminal_reason, "IDENTITY_MISMATCH")
        self.assertEqual(orch.envelopes, [])

    # -- 6. runner crash mid-case --------------------------------------
    def test_06_runner_crash_yields_no_semantic_result(self):
        runner = RunnerModel(crash_on_case="vfpu_trap")
        transport = FakeTransport(runner)
        orch = Orchestrator(transport, max_l1_recoveries=0)
        orch.qualify()
        orch.run_case("vfpu_trap")
        self.assertEqual(orch.envelopes, [])
        self.assertIn("L3", " ".join(orch.recovery_events))
        self.assertEqual(orch.terminal_reason, "PHYSICAL_INTERVENTION_REQUIRED")

    # -- 8. connection drop --------------------------------------------
    def test_08_connection_drop_is_transport_fault_not_semantic(self):
        orch, _, transport = self.build()
        orch.qualify()
        transport.drop_after_send = True
        orch.run_case("vfpu_a_02")
        # The drop is a TRANSPORT fault: no semantic result may appear, and the
        # bounded ladder records the L1 recovery instead of fabricating output.
        self.assertEqual(orch.envelopes, [])
        self.assertEqual(orch.terminal_reason, "RECOVERED_WITH_L1")
        self.assertTrue(any(e.startswith("L0:") for e in orch.recovery_events))

    # -- 15. partial result rejected ------------------------------------
    def test_15_partial_result_fails_evidence_check(self):
        orch, _, _ = self.build(partial_result=True)
        orch.qualify()
        orch.run_case("vfpu_b_07")
        self.assertEqual(orch.envelopes, [])
        self.assertEqual(orch.terminal_reason, "EVIDENCE_REJECTED")

    # -- 2/3. echo-only peer and hanging USB server ---------------------
    def test_02_command_echo_never_counts_as_response(self):
        orch, _, transport = self.build()
        transport.echo_without_frame = True
        with self.assertRaises(CommandTimeout):
            orch.qualify()
        self.assertFalse(orch.qualified)

    def test_03_usb_server_waiting_forever_times_out_cleanly(self):
        orch, _, transport = self.build()
        transport.usb_server_hangs = True
        with self.assertRaises(CommandTimeout):
            orch.qualify()
        self.assertEqual(orch.envelopes, [])

    # -- 9/10. recovery success and exhaustion --------------------------
    def test_09_single_fault_recovers_within_budget(self):
        runner = RunnerModel()
        transport = FakeTransport(runner)
        orch = Orchestrator(transport, max_l1_recoveries=1)
        orch.qualify()
        transport.drop_after_send = True
        orch.run_case("vfpu_c_03")
        self.assertEqual(orch.terminal_reason, "RECOVERED_WITH_L1")
        self.assertIn("L1: host stack restart", " ".join(orch.recovery_events))

    def test_10_recovery_exhaustion_stops_at_physical_intervention(self):
        runner = RunnerModel()
        transport = FakeTransport(runner)
        orch = Orchestrator(transport, max_l1_recoveries=0)
        orch.qualify()
        transport.drop_after_send = True
        orch.run_case("vfpu_d_09")
        self.assertEqual(orch.terminal_reason, "PHYSICAL_INTERVENTION_REQUIRED")

    # -- 5/11. duplicate/stale rejection ---------------------------------
    def test_11_result_from_prior_epoch_is_discarded_as_stale(self):
        stale = frame_encode(
            RESULT, 1,
            b"status=OK\nresult_len=0\nresult_sha256=" + _sha(b"").encode() +
            b"\n\nglobal_epoch=epoch-STALE\n",
        )
        orch, _, transport = self.build()
        orch.qualify()
        orch.run_case("vfpu_a_04")
        self.assertEqual(len(orch.envelopes), 1)
        # A stale RESULT from a previous epoch arrives ahead of the real
        # response; the orchestrator must skip it and still get the answer.
        transport.inject_after_next_request = stale
        transport.stale_first = True
        orch.run_case("vfpu_a_05")
        self.assertEqual(len(orch.envelopes), 2)
        self.assertEqual([e["GLOBAL_EPOCH"] for e in orch.envelopes], [EPOCH, EPOCH])
        self.assertEqual(orch.terminal_reason, None)

    # -- 16. abrupt host-side transport death -----------------------------
    def test_16_host_process_death_surfaces_as_transport_dead(self):
        orch, runner, _transport = self.build()
        orch.qualify()
        runner.alive = False
        # The orchestrator absorbs the dead link through the ladder; the
        # invariants are that nothing semantic is emitted and the epoch stops.
        orch.run_case("vfpu_e_11")
        self.assertEqual(orch.envelopes, [])
        self.assertIn(orch.terminal_reason,
                      ("PHYSICAL_INTERVENTION_REQUIRED", "RECOVERED_WITH_L1"))
        self.assertNotEqual(orch.state, "VERIFY_RESULT")

    def test_17_real_adapter_runs_multiple_cases_unloads_and_binds_console_metadata(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-sim-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id in ("transport-write", "model-profile"):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(case_id.encode())
                cases.append(CampaignCase(case_id, binary, 1.25))
            transport = SimulatedPsplinkTransport()
            transport.host0_root = scratch
            runner = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            )
            report = runner.run(cases)

        self.assertTrue(transport.started)
        self.assertTrue(transport.stopped)
        self.assertIsNone(report["terminal_reason"])
        self.assertEqual(
            [item["CASE_ID"] for item in report["envelopes"]],
            ["transport-write", "model-profile"],
        )
        for envelope in report["envelopes"]:
            self.assertEqual(envelope["PHYSICAL_MODEL_LABEL"], "PSP-3000-04g")
            self.assertEqual(envelope["FW"], "6.6.1")
            self.assertEqual(envelope["INTERPRETED_MODEL_FAMILY"], "PSP-3000")
            self.assertEqual(envelope["MODEL_INTERPRETATION_RULE"], "PSPSDK_PMODEL_ORDINAL_V1")
            self.assertEqual(envelope["MODEL_IDENTITY_AGREEMENT"], "AGREES")
            self.assertTrue(envelope["ACCEPTANCE_ELIGIBLE"])
            self.assertNotIn("SERIAL", " ".join(envelope).upper())
        self.assertEqual(report["envelopes"][0]["SOFTWARE_MODEL_RAW_VALUE"], "3")
        self.assertEqual(report["envelopes"][1]["SOFTWARE_MODEL_RAW_VALUE"], "0x3")
        commands = [command for command, _timeout in transport.commands]
        self.assertEqual(commands.count("modstun 0x04280001"), 2)
        self.assertEqual(commands.count("modinfo 0x04280001"), 2)
        self.assertEqual(
            [envelope["TEARDOWN_CHECK"]["modstun_reply"] for envelope in report["envelopes"]],
            ["Module Stop/Unload 0x00000000/0x04280001 Status 0xDEADBEEF"] * 2,
        )
        self.assertEqual(
            [timeout for command, timeout in transport.commands if command.startswith("ldstart")],
            [1.25, 1.25],
        )

    def test_simulated_psplink_snapshots_follow_load_failed_unload_and_reset(self):
        transport = SimulatedPsplinkTransport(fail_modstun=True)

        def snapshot():
            thread_result = transport.run("thlist", 1.0)
            memory_result = transport.run("meminfo", 1.0)
            module_result = transport.run("modlist", 1.0)
            self.assertEqual((thread_result[0], memory_result[0], module_result[0]), (0, 0, 0))
            return (
                parse_psplink_thread_snapshot(thread_result[1]),
                parse_psplink_meminfo(memory_result[1]),
                parse_psplink_module_list(module_result[1]),
            )

        s0 = snapshot()
        loaded = transport.run("ldstart host0:/synthetic-probe.prx", 1.0)
        self.assertIn("UID: 0x04280001", loaded[1])
        s1 = snapshot()
        self.assertEqual(s1[0] - s0[0], {transport._probe_thread})
        self.assertNotEqual(s1[1], s0[1])
        self.assertEqual(s1[2] - s0[2], {("0x04280001", "NAKAGAWA_PSP_ORACLE")})
        threads = transport.run("modinfo 0x04280001 t", 1.0)
        self.assertEqual(
            parse_psplink_module_threads(threads[1], "0x04280001"),
            {transport._probe_thread},
        )

        unload = transport.run("modstun 0x04280001", 1.0)
        self.assertEqual(unload[0], 1)
        failed_s2 = snapshot()
        self.assertEqual(failed_s2, s1)
        self.assertEqual(transport.run("modinfo 0x04280001", 1.0)[0], 0)

        reset = transport.run("reset", 1.0)
        self.assertEqual(reset[3], "PROCESS_EXITED")
        self.assertEqual(snapshot(), s0)
        self.assertEqual(transport.run("modinfo 0x04280001", 1.0)[0], 1)
        with self.assertRaisesRegex(AssertionError, "unexpected PSPLINK command"):
            transport.run("some-unmodeled-command", 1.0)

    def test_failed_teardown_reset_fallback_stays_unqualified(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-teardown-fallback-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            binary = scratch / "transport-write.prx"
            binary.write_bytes(b"synthetic PRX")
            transport = SimulatedPsplinkTransport(
                fail_modstun_cases={"transport-write"}
            )
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run([CampaignCase("transport-write", binary, 1.0)])

        self.assertIsNone(report["terminal_reason"])
        self.assertIn("reset", [command for command, _timeout in transport.commands])
        failed = report["envelopes"][0]
        self.assertEqual(failed["TEARDOWN_CHECK"]["status"], "FAIL")
        self.assertTrue(any(
            "post-unload module set differs from S0" in issue
            for issue in failed["TEARDOWN_CHECK"]["issues"]
        ))
        self.assertFalse(failed["ACCEPTANCE_ELIGIBLE"])
        self.assertIn("loaded module was not proven unloaded", failed["ACCEPTANCE_BLOCKERS"])

    def test_campaign_model_profile_mismatch_is_refused_without_rewriting_raw_value(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(
            prefix="runner-model-identity-mismatch-", dir=fixture_dir
        ) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id in ("transport-write", "model-profile"):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(b"synthetic PRX")
                cases.append(CampaignCase(case_id, binary, 1.0))
            transport = SimulatedPsplinkTransport(model_profile_raw_value="0x4")
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-series",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run(cases)

        envelope = report["envelopes"][1]
        self.assertEqual(envelope["SOFTWARE_MODEL_RAW_VALUE"], "0x4")
        self.assertEqual(envelope["INTERPRETED_MODEL_FAMILY"], "PSP-N1000")
        self.assertEqual(envelope["MODEL_IDENTITY_AGREEMENT"], "DISAGREES")
        self.assertFalse(envelope["ACCEPTANCE_ELIGIBLE"])
        self.assertIn(
            "device model-profile out0=0x4 does not match",
            " | ".join(envelope["ACCEPTANCE_BLOCKERS"]),
        )

    def test_campaign_waits_for_complete_fpu_host0_stream_before_unload(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-fpu-completion-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id in ("transport-write", "fpu-vector"):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(b"synthetic PRX")
                cases.append(CampaignCase(case_id, binary, 1.0))

            partial = _campaign_fpu_stream(FPU_EXPECTED_CELLS[:5])
            complete = _campaign_fpu_stream().replace(
                "case_id=fpu-cvt-rm0 status=PASS",
                "case_id=fpu-cvt-rm0 status=FAIL",
                1,
            )
            transport = SimulatedPsplinkTransport(
                stdout_record_cases={"fpu-vector"},
                host0_log_contents={"fpu-vector": partial},
            )
            transport.host0_root = scratch
            wait_observations: list[tuple[str, ...]] = []
            real_wait = run_psplink_module._wait_for_host0_output

            def finish_fpu_stream_during_wait(
                path, timeout, *, not_before_ns=None, ready, include_mtime=False
            ):
                if path.name == "fpu_vector_log.txt":
                    self.assertFalse(ready(path.read_text(encoding="utf-8")))
                    wait_observations.append(
                        tuple(command for command, _ in transport.commands)
                    )
                    # Model host0 finishing its deferred write only after the
                    # production runner has observed and rejected the prefix.
                    path.write_text(complete, encoding="utf-8")
                return real_wait(
                    path,
                    timeout,
                    not_before_ns=not_before_ns,
                    ready=ready,
                    include_mtime=include_mtime,
                )

            with patch(
                "psp_oracle.run_psplink._wait_for_host0_output",
                side_effect=finish_fpu_stream_during_wait,
            ):
                report = PsplinkCampaignRunner(
                    transport,
                    console_model="PSP-3000-04g",
                    source_commit=SOURCE_COMMIT,
                    model_code=3,
                ).run(cases)

        self.assertEqual(len(wait_observations), 1)
        self.assertEqual(wait_observations[0][-1], "ldstart host0:/fpu-vector.prx")
        fpu_start = wait_observations[0].index("ldstart host0:/fpu-vector.prx")
        self.assertFalse(
            any(command.startswith("modstun ") for command in wait_observations[0][fpu_start + 1 :])
        )
        self.assertEqual(transport.host0_record_counts_at_unload["fpu-vector"], 16)
        envelope = report["envelopes"][1]
        self.assertEqual(envelope["STREAM_COMPLETENESS_CONTRACT"], "strict-golden-sequence")
        self.assertFalse(envelope["ACCEPTANCE_ELIGIBLE"])
        self.assertIn("status=FAIL", envelope["RAW_RESULT"])
        self.assertIn("one or more scalar result records did not pass", envelope["ACCEPTANCE_BLOCKERS"])

    def test_campaign_incomplete_fpu_timeout_still_unloads_and_stays_unqualified(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-fpu-timeout-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id, timeout in (("transport-write", 1.0), ("fpu-vector", 0.001)):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(b"synthetic PRX")
                cases.append(CampaignCase(case_id, binary, timeout))

            transport = SimulatedPsplinkTransport(
                stdout_record_cases={"fpu-vector"},
                host0_log_contents={
                    "fpu-vector": _campaign_fpu_stream(FPU_EXPECTED_CELLS[:5])
                },
            )
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run(cases)

        envelope = report["envelopes"][1]
        commands = [command for command, _timeout in transport.commands]
        fpu_start = commands.index("ldstart host0:/fpu-vector.prx")
        unload = commands.index("modstun 0x04280001", fpu_start + 1)
        self.assertLess(fpu_start, unload)
        self.assertEqual(transport.host0_record_counts_at_unload["fpu-vector"], 5)
        self.assertEqual(envelope["STREAM_COMPLETENESS_CONTRACT"], "strict-golden-sequence")
        self.assertEqual(envelope["QUALIFICATION_STATUS"], "UNQUALIFIED")
        self.assertFalse(envelope["ACCEPTANCE_ELIGIBLE"])
        self.assertTrue(
            any(
                "reached no completion marker" in blocker
                for blocker in envelope["QUALIFICATION_BLOCKERS"]
            )
        )
        self.assertTrue(
            any(
                "strict protocol validation" in blocker
                for blocker in envelope["QUALIFICATION_BLOCKERS"]
            )
        )

    def test_campaign_rejects_nonregular_host0_result_before_reading(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-unsafe-host0-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id in ("transport-write", "fpu-vector"):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(b"synthetic PRX")
                cases.append(CampaignCase(case_id, binary, 1.0))

            transport = SimulatedPsplinkTransport(stdout_record_cases={"fpu-vector"})
            transport.host0_root = scratch
            real_wait = run_psplink_module._wait_for_host0_output
            unsafe_read_attempts: list[Path] = []
            original_read_bytes = Path.read_bytes

            def replace_result_with_directory(path, timeout, **kwargs):
                if path.name != "fpu_vector_log.txt":
                    return real_wait(path, timeout, **kwargs)
                path.unlink()
                path.mkdir()
                (path / "unsafe-target.txt").write_text(
                    "must not be read", encoding="utf-8"
                )

                def guarded_read(candidate):
                    if candidate == path:
                        unsafe_read_attempts.append(candidate)
                    return original_read_bytes(candidate)

                try:
                    with patch.object(Path, "read_bytes", new=guarded_read):
                        return real_wait(path, timeout, **kwargs)
                except UnsafeHost0OutputError:
                    raise
                raise AssertionError("unsafe host0 path should be rejected by the wait")

            with patch(
                "psp_oracle.run_psplink._wait_for_host0_output",
                side_effect=replace_result_with_directory,
            ):
                report = PsplinkCampaignRunner(
                    transport,
                    console_model="PSP-3000-04g",
                    source_commit=SOURCE_COMMIT,
                    model_code=3,
                ).run(cases)

        envelope = report["envelopes"][1]
        commands = [command for command, _timeout in transport.commands]
        fpu_start = commands.index("ldstart host0:/fpu-vector.prx")
        self.assertIn("modstun 0x04280001", commands[fpu_start + 1 :])
        self.assertEqual(unsafe_read_attempts, [])
        self.assertEqual(envelope["QUALIFICATION_STATUS"], "UNQUALIFIED")
        self.assertFalse(envelope["ACCEPTANCE_ELIGIBLE"])
        self.assertIn("not a regular file", " ".join(envelope["QUALIFICATION_BLOCKERS"]))
        self.assertNotIn("must not be read", envelope["RAW_RESULT"])

    def test_host0_wait_rejects_symlink_before_read(self):
        with tempfile.TemporaryDirectory(prefix="runner-host0-symlink-") as scratch_name:
            scratch = Path(scratch_name)
            target = scratch / "target.txt"
            target.write_text("private-target-marker", encoding="utf-8")
            link = scratch / "result.txt"
            try:
                link.symlink_to(target)
            except (OSError, NotImplementedError) as exc:
                self.skipTest(f"symbolic links are unavailable in this environment: {exc}")

            opens: list[Path] = []
            original_open = os.open

            def guarded_open(candidate, flags, *args, **kwargs):
                if Path(candidate) == link:
                    opens.append(Path(candidate))
                    raise AssertionError("unsafe host0 symlink must not be opened")
                return original_open(candidate, flags, *args, **kwargs)

            with patch("psp_oracle.run_psplink.os.open", side_effect=guarded_open):
                with self.assertRaises(UnsafeHost0OutputError):
                    _wait_for_host0_output(
                        link, 0.01, ready=lambda _text: True
                    )
            self.assertEqual(opens, [])

    def test_host0_wait_rejects_replacement_with_pre_run_mtime(self):
        with tempfile.TemporaryDirectory(prefix="runner-host0-replacement-") as scratch_name:
            scratch = Path(scratch_name)
            result_path = scratch / "result.txt"
            replacement_path = scratch / "replacement.txt"
            result_path.write_text("complete stable result\n", encoding="utf-8")
            run_started_ns = result_path.stat().st_mtime_ns
            replacement_path.write_text("complete stable result\n", encoding="utf-8")
            os.utime(replacement_path, ns=(1, 1))
            replaced = False

            def swap_in_stale_file(_delay):
                nonlocal replaced
                if not replaced:
                    os.replace(replacement_path, result_path)
                    replaced = True

            with patch("psp_oracle.run_psplink.time.sleep", side_effect=swap_in_stale_file):
                with self.assertRaises(UnsafeHost0OutputError):
                    _wait_for_host0_output(
                        result_path,
                        0.01,
                        not_before_ns=run_started_ns,
                        ready=lambda _text: True,
                    )
            self.assertTrue(replaced)

    def test_host0_wait_rejects_replacement_between_stat_and_open(self):
        with tempfile.TemporaryDirectory(prefix="runner-host0-open-race-") as scratch_name:
            scratch = Path(scratch_name)
            result_path = scratch / "result.txt"
            replacement_path = scratch / "replacement.txt"
            result_path.write_text("original complete result\n", encoding="utf-8")
            replacement_path.write_text("replacement complete result\n", encoding="utf-8")
            replaced = False
            original_open = os.open

            def replace_before_open(path, flags, *args, **kwargs):
                nonlocal replaced
                if Path(path) == result_path and not replaced:
                    os.replace(replacement_path, result_path)
                    replaced = True
                return original_open(path, flags, *args, **kwargs)

            with patch("psp_oracle.run_psplink.os.open", side_effect=replace_before_open):
                with self.assertRaises(UnsafeHost0OutputError):
                    _wait_for_host0_output(
                        result_path, 0.1, ready=lambda _text: True
                    )
            self.assertTrue(replaced)

    def test_host0_snapshot_rejects_replacement_between_stat_and_open(self):
        with tempfile.TemporaryDirectory(prefix="runner-host0-snapshot-race-") as scratch_name:
            scratch = Path(scratch_name)
            result_path = scratch / "result.txt"
            replacement_path = scratch / "replacement.txt"
            result_path.write_text("original partial result\n", encoding="utf-8")
            replacement_path.write_text("replacement partial result\n", encoding="utf-8")
            replaced = False
            original_open = os.open

            def replace_before_open(path, flags, *args, **kwargs):
                nonlocal replaced
                if Path(path) == result_path and not replaced:
                    os.replace(replacement_path, result_path)
                    replaced = True
                return original_open(path, flags, *args, **kwargs)

            with patch("psp_oracle.run_psplink.os.open", side_effect=replace_before_open):
                text, mtime_ns, problem = _snapshot_host0_output(result_path)
            self.assertTrue(replaced)
            self.assertIsNone(text)
            self.assertIsNone(mtime_ns)
            self.assertIn("changed during capture", problem or "")

    def test_unregistered_campaign_row_is_never_a_complete_stream(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        record = (
            CAMPAIGN_META
            + "NAKAGAWA_PSP_TEST schema=1 test_id=SYNTHETIC "
            "case_id=future-case status=PASS result=0x00000001\n"
        )
        with tempfile.TemporaryDirectory(prefix="runner-unregistered-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id in ("transport-write", "future-case"):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(b"synthetic PRX")
                cases.append(CampaignCase(case_id, binary, 0.01))

            transport = SimulatedPsplinkTransport(
                stdout_record_cases={"future-case"},
                host0_log_contents={"future-case": record},
            )
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run(cases)

        envelope = report["envelopes"][1]
        self.assertEqual(
            envelope["STREAM_COMPLETENESS_CONTRACT"],
            "unregistered-no-completion-contract",
        )
        self.assertFalse(envelope["ACCEPTANCE_ELIGIBLE"])
        self.assertTrue(
            any(
                "completion contract for future-case is unregistered (in the works: issue #352)"
                in blocker
                for blocker in envelope["QUALIFICATION_BLOCKERS"]
            )
        )

    def test_campaign_known_stream_contracts_reject_truncation_and_wrong_single_row(self):
        with self.assertRaises(PspProtocolError):
            _parse_campaign_records(
                _campaign_fpu_stream(FPU_EXPECTED_CELLS[:5]), "fpu-vector"
            )
        with self.assertRaises(PspProtocolError):
            _parse_campaign_records(
                _campaign_spec_stream(
                    CACHE_SPEC, CACHE_SPEC.ordered_cases[:-1]
                ),
                "cache-alias",
            )
        with self.assertRaises(PspProtocolError):
            _parse_campaign_records(
                _campaign_spec_stream(IO_SPEC, IO_SPEC.ordered_cases[:-1]), "io-matrix"
            )

        transport_row = (
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-TRANSPORT-001 "
            "case_id=host0-write-readback status=PASS result=0x0\n"
        )
        with self.assertRaises(PspProtocolError):
            _parse_campaign_records(CAMPAIGN_META + transport_row + transport_row, "transport-write")
        with self.assertRaises(PspProtocolError):
            _parse_campaign_records(
                CAMPAIGN_META
                + "NAKAGAWA_PSP_TEST schema=1 test_id=SYNTHETIC case_id=model-profile "
                "status=PASS result=0x0\n",
                "model-profile",
            )
        unregistered = (
            CAMPAIGN_META
            + "NAKAGAWA_PSP_TEST schema=1 test_id=SYNTHETIC "
            "case_id=future-case status=PASS result=0x00000001\n"
        )
        self.assertEqual(
            run_psplink_module._campaign_completeness_contract("future-case"),
            "unregistered-no-completion-contract",
        )
        self.assertFalse(
            run_psplink_module._campaign_stream_complete(unregistered, "future-case")
        )

        self.assertEqual(
            len(_parse_campaign_records(_campaign_fpu_stream(), "fpu-vector").results),
            16,
        )
        self.assertEqual(
            len(_parse_campaign_records(_campaign_spec_stream(CACHE_SPEC), "cache-alias").results),
            CACHE_SPEC.record_count,
        )
        self.assertEqual(
            len(_parse_campaign_records(_campaign_spec_stream(IO_SPEC), "io-matrix").results),
            IO_SPEC.record_count,
        )

    def test_new_probe_cases_register_strict_runner_completeness(self):
        for case_id in ("audio-query", "ge-nan", "dma-cells", "delay-zero"):
            with self.subTest(case_id=case_id):
                self.assertEqual(
                    run_psplink_module._campaign_completeness_contract(case_id),
                    "strict-golden-sequence",
                )
                self.assertFalse(run_psplink_module._campaign_stream_complete(
                    CAMPAIGN_META, case_id
                ))

        def invalid_cell_row(case_id, *, tier, api, endpoint, delta=0,
                             prefix=0, matches=0, post=0, band=0,
                             source_addr=0x08811000,
                             destination_addr=0x08822000):
            fields = {
                "result": 0, "rc": 0, "P": prefix, "matches": matches,
                "guards_outside": 0, "post_guard": post,
                "overflow_band": band, "source_intact": 1,
                "setup_mask": 0xF, "K": 0xC000, "delta": delta,
                "api": api, "endpoint": endpoint,
                "cache_discipline": 1, "tier": tier, "executed": 1,
                "payload_mutations": 0, "source_addr": source_addr,
                "destination_addr": destination_addr,
            }
            encoded = " ".join(
                f"{key}=0x{value:08x}" if isinstance(value, int)
                else f"{key}={value}"
                for key, value in fields.items()
            )
            return (
                "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-DMAC-001 "
                f"case_id={case_id} status=PASS {encoded}\n"
            )

        for campaign_case, launch in DMAC_INVALID_CASES.items():
            with self.subTest(case_id=campaign_case):
                self.assertEqual(
                    run_psplink_module._campaign_completeness_contract(campaign_case),
                    "strict-dmac-invalid-cell-records",
                )
                self.assertFalse(run_psplink_module._campaign_stream_complete(
                    CAMPAIGN_META, campaign_case
                ))
                rows = []
                if launch.tier == "S":
                    for api in ("memcpy", "try"):
                        for shape, endpoint in (("a", "dst"), ("b", "src"),
                                                ("c", "both"), ("d", "dst")):
                            rows.append(invalid_cell_row(
                                f"invalid-tail-s0-{shape}-{api}", tier="S",
                                api=api, endpoint=endpoint,
                                source_addr=0 if shape == "b" else 0x08811000,
                                destination_addr=(
                                    0 if shape == "a" else
                                    0xFFFFFFFF if shape == "d" else 0x08822000
                                ),
                            ))
                else:
                    for delta in (1, 4, 0x1000, 0x2000):
                        requested = 0xC000 + delta
                        post = min(delta, 0x1000) if launch.endpoint == "dst" else 0
                        band = max(delta - 0x1000, 0) if launch.endpoint == "dst" else 0
                        rows.append(invalid_cell_row(
                            f"invalid-tail-{launch.cell}-delta-{delta:04x}",
                            tier="B", api=launch.api, endpoint=launch.endpoint,
                            delta=delta, prefix=requested, matches=requested,
                            post=post, band=band,
                        ))
                complete = CAMPAIGN_META + "".join(rows)
                self.assertTrue(run_psplink_module._campaign_stream_complete(
                    complete, campaign_case
                ))
                self.assertEqual(
                    len(_parse_campaign_records(complete, campaign_case).results),
                    len(launch.case_ids),
                )
                malformed = complete.replace(
                    "guards_outside=0x00000000",
                    "guards_outside=0x00000001",
                    1,
                )
                self.assertFalse(
                    run_psplink_module._campaign_stream_complete(
                        malformed, campaign_case
                    )
                )

    def test_campaign_mbx_delete_wait_requires_exact_complete_stream(self):
        complete = _campaign_mbx_delete_wait_stream()
        truncated = _campaign_mbx_delete_wait_stream(
            MBX_DELETE_WAIT_SPEC.ordered_cases[:-1]
        )

        self.assertEqual(
            run_psplink_module._campaign_completeness_contract("mbx-delete-wait"),
            "strict-golden-sequence",
        )
        self.assertTrue(
            run_psplink_module._campaign_stream_complete(
                complete, "mbx-delete-wait"
            )
        )
        self.assertFalse(
            run_psplink_module._campaign_stream_complete(
                truncated, "mbx-delete-wait"
            )
        )
        with self.assertRaises(PspProtocolError):
            _parse_campaign_records(truncated, "mbx-delete-wait")
        self.assertEqual(
            len(_parse_campaign_records(complete, "mbx-delete-wait").results),
            MBX_DELETE_WAIT_SPEC.record_count,
        )

    def test_campaign_host0_only_records_qualify_without_stdout_records(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-host0-only-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            binary = scratch / "transport-write.prx"
            binary.write_bytes(b"synthetic PRX")
            transport = SimulatedPsplinkTransport(
                stdout_record_cases={"transport-write"}
            )
            transport.host0_root = scratch
            with patch(
                "psp_oracle.run_psplink._check_source_tree", return_value=None
            ):
                report = PsplinkCampaignRunner(
                    transport,
                    console_model="PSP-3000-04g",
                    source_commit=SOURCE_COMMIT,
                    model_code=3,
                ).run([CampaignCase("transport-write", binary, 1.0)])

        envelope = report["envelopes"][0]
        self.assertTrue(envelope["ACCEPTANCE_ELIGIBLE"])
        self.assertEqual(envelope["QUALIFICATION_STATUS"], "QUALIFIED")
        self.assertTrue(envelope["HOST0_LOG_FRESH"])
        self.assertIn("case_id=host0-write-readback", envelope["RAW_RESULT"])

    def test_campaign_healthy_transport_write_then_second_case_same_session(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(
            prefix="runner-second-healthy-case-", dir=fixture_dir
        ) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id in ("transport-write", "model-profile"):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(case_id.encode())
                cases.append(CampaignCase(case_id, binary, 1.0))
            transport = SimulatedPsplinkTransport()
            transport.host0_root = scratch
            with patch(
                "psp_oracle.run_psplink._check_source_tree", return_value=None
            ):
                report = PsplinkCampaignRunner(
                    transport,
                    console_model="PSP-3000-04g",
                    source_commit=SOURCE_COMMIT,
                    model_code=3,
                ).run(cases)

            self.assertFalse((scratch / "nakagawa_transport_write.bin").exists())

        self.assertIsNone(report["terminal_reason"])
        self.assertEqual(
            [command for command, _timeout in transport.commands if command.startswith("ldstart ")],
            ["ldstart host0:/transport-write.prx", "ldstart host0:/model-profile.prx"],
        )
        self.assertEqual(
            [envelope["CASE_ID"] for envelope in report["envelopes"]],
            ["transport-write", "model-profile"],
        )
        for envelope in report["envelopes"]:
            with self.subTest(case_id=envelope["CASE_ID"]):
                self.assertEqual(envelope["TEARDOWN_CHECK"]["status"], "PASS")
                self.assertTrue(envelope["HOST0_LOG_FRESH"])
                self.assertTrue(envelope["ACCEPTANCE_ELIGIBLE"])

    def test_campaign_missing_second_host0_roundtrip_fails_closed(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(
            prefix="runner-second-roundtrip-fault-", dir=fixture_dir
        ) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id in ("transport-write", "model-profile"):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(case_id.encode())
                cases.append(CampaignCase(case_id, binary, 1.0))
            transport = SimulatedPsplinkTransport(
                fail_host0_roundtrip_cases={"model-profile"}
            )
            transport.host0_root = scratch
            with patch(
                "psp_oracle.run_psplink._check_source_tree", return_value=None
            ):
                report = PsplinkCampaignRunner(
                    transport,
                    console_model="PSP-3000-04g",
                    source_commit=SOURCE_COMMIT,
                    model_code=3,
                ).run(cases)

            self.assertFalse((scratch / "nakagawa_transport_write.bin").exists())

        first, second = report["envelopes"]
        self.assertEqual(first["TEARDOWN_CHECK"]["status"], "PASS")
        self.assertTrue(first["ACCEPTANCE_ELIGIBLE"])
        self.assertEqual(second["TEARDOWN_CHECK"]["status"], "FAIL")
        self.assertIn(
            "host0 round-trip failed after unload",
            second["TEARDOWN_CHECK"]["issues"],
        )
        self.assertFalse(second["ACCEPTANCE_ELIGIBLE"])
        self.assertEqual(report["terminal_reason"], "HOST0_ROUNDTRIP_FAILED")

    def test_campaign_device_identity_is_compared_before_canonicalization(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        for device_commit, expected_binding, expected_eligible, expected_blocker in (
            (SOURCE_COMMIT, "MATCH", True, None),
            ("f" * 40, "MISMATCH", False, "IDENTITY_MISMATCH"),
            ("0" * 40, "PLACEHOLDER", False, "IDENTITY_NOT_BOUND"),
            (None, "NOT_REPORTED", False, "IDENTITY_NOT_BOUND"),
        ):
            with self.subTest(device_commit=device_commit):
                with tempfile.TemporaryDirectory(
                    prefix="runner-device-identity-", dir=fixture_dir
                ) as scratch_name:
                    scratch = Path(scratch_name)
                    binary = scratch / "transport-write.prx"
                    binary.write_bytes(b"synthetic transport PRX")
                    transport = SimulatedPsplinkTransport(
                        device_source_commit=device_commit
                    )
                    transport.host0_root = scratch
                    with patch(
                        "psp_oracle.run_psplink._check_source_tree", return_value=None
                    ):
                        report = PsplinkCampaignRunner(
                            transport,
                            console_model="PSP-3000-04g",
                            source_commit=SOURCE_COMMIT,
                            model_code=3,
                        ).run([CampaignCase("transport-write", binary, 1.0)])

                envelope = report["envelopes"][0]
                self.assertEqual(
                    envelope["DEVICE_REPORTED_SOURCE_COMMIT"], device_commit
                )
                self.assertEqual(
                    envelope["SOURCE_COMMIT_BINDING"], expected_binding
                )
                self.assertEqual(
                    envelope["ACCEPTANCE_ELIGIBLE"], expected_eligible
                )
                if expected_blocker is None:
                    self.assertEqual(envelope["DEVICE_IDENTITY_BLOCKERS"], [])
                else:
                    self.assertIn(
                        expected_blocker,
                        " | ".join(envelope["ACCEPTANCE_BLOCKERS"]),
                    )

    def test_campaign_stale_host0_log_is_cleared_and_rejected_by_mtime(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-stale-host0-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            binary = scratch / "transport-write.prx"
            binary.write_bytes(b"synthetic PRX")
            log = scratch / "transport_write_log.txt"
            log.write_text("stale result must not qualify\n", encoding="utf-8")
            transport = SimulatedPsplinkTransport(stale_host0_mtime=True)
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run([CampaignCase("transport-write", binary, 1.0)])

        envelope = report["envelopes"][0]
        self.assertFalse(transport.stale_log_present_at_load)
        self.assertFalse(envelope["HOST0_LOG_FRESH"])
        self.assertEqual(envelope["QUALIFICATION_STATUS"], "UNQUALIFIED")
        self.assertIn(
            "host0 log modification time is outside this case's run window",
            envelope["QUALIFICATION_BLOCKERS"],
        )
        self.assertNotIn("stale result must not qualify", envelope["RAW_RESULT"])

    def test_campaign_dirty_source_tree_cannot_qualify_records(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-dirty-source-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            binary = scratch / "transport-write.prx"
            binary.write_bytes(b"synthetic PRX")
            transport = SimulatedPsplinkTransport()
            transport.host0_root = scratch
            with patch(
                "psp_oracle.run_psplink._check_source_tree",
                return_value="source worktree is dirty",
            ):
                report = PsplinkCampaignRunner(
                    transport,
                    console_model="PSP-3000-04g",
                    source_commit=SOURCE_COMMIT,
                    model_code=3,
                ).run([CampaignCase("transport-write", binary, 1.0)])

        envelope = report["envelopes"][0]
        self.assertEqual(envelope["SOURCE_TREE_STATUS"], "UNQUALIFIED")
        self.assertEqual(envelope["QUALIFICATION_STATUS"], "UNQUALIFIED")
        self.assertIn("source worktree is dirty", envelope["QUALIFICATION_BLOCKERS"])
        self.assertFalse(envelope["ACCEPTANCE_ELIGIBLE"])

    def test_campaign_stdout_host0_disagreement_is_a_named_failure(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-channel-mismatch-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id in ("transport-write", "probe"):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(b"synthetic PRX")
                cases.append(CampaignCase(case_id, binary, 1.0))
            transport = SimulatedPsplinkTransport(
                stdout_result_overrides={"probe": "0x2"}
            )
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run(cases)

        envelope = report["envelopes"][1]
        self.assertEqual(envelope["QUALIFICATION_STATUS"], "UNQUALIFIED")
        self.assertIn(
            "stdout and host0 schema records disagree",
            envelope["QUALIFICATION_BLOCKERS"],
        )
        self.assertFalse(envelope["ACCEPTANCE_ELIGIBLE"])

    def test_campaign_dmac_case_ids_map_to_probe_owned_log_names(self):
        root = Path("host0-root")
        self.assertEqual(
            _campaign_host0_log_path(root, "dma-size-matrix").name,
            "dmac_size_matrix_log.txt",
        )
        self.assertEqual(
            _campaign_host0_log_path(root, "dma-invalid-tail-memcpy-dst").name,
            "dmac_invalid_tail_memcpy_dst_log.txt",
        )
        self.assertEqual(
            _campaign_host0_log_path(root, "dma-invalid-tail-s0").name,
            "dmac_invalid_tail_s0_log.txt",
        )
        self.assertEqual(
            _campaign_host0_log_path(
                root, "dmac-size-matrix-size-0x0000bfff"
            ).name,
            "dmac_size_matrix_cell_log.txt",
        )

    def test_campaign_dmac_size_case_validates_only_its_two_api_records(self):
        size = 0xBFFF
        allocation_bytes = (size + 0x2FFF) & ~0xFFF
        metadata = (
            "NAKAGAWA_PSP_META schema=1 source=psp model=PSP-3000 "
            "firmware=6.61-ARK binary_sha256=" + "a" * 64 + " source_commit=" + "b" * 40 + "\n"
        )
        records = []
        for api, name in enumerate(("memcpy", "try")):
            records.append(
                "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-DMAC-001 "
                f"case_id=size-matrix-{name}-0x{size:08x} status=PASS "
                f"result=0x0 out0=0x{size:x} out1=0x{size:x} out2=0x0 "
                f"out3=0x1 out4=0x10 out5=0x{api:x} out6=0x3 out7=0x0 "
                f"out8=0x0 out9=0x0 out10=0x0 out11=0x1000 "
                f"out12=0x{allocation_bytes:x} out13=0x2 out14=0x1000 "
                "out15=0x8801000 out16=0x8c01000 out17=0x1 out18=0x2\n"
            )
        case_id = f"dmac-size-matrix-size-0x{size:08x}"
        parsed = _parse_campaign_records(metadata + "".join(records), case_id)
        self.assertEqual(len(parsed.results), 2)
        with self.assertRaises(ValueError):
            _parse_campaign_records(metadata + records[0], case_id)

    def test_18_real_adapter_bounds_timeouts_and_keeps_partial_output_nonsemantic(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-timeout-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id in ("transport-write", "timeout", "model-profile"):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(case_id.encode())
                cases.append(CampaignCase(case_id, binary, 0.75))
            transport = SimulatedPsplinkTransport(timeout_cases={"timeout"})
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run(cases)

        self.assertEqual(len(report["envelopes"]), 2)
        _transport, timed_out = report["envelopes"]
        self.assertEqual(timed_out["PROCESS_STATUS"], "TIMEOUT")
        self.assertFalse(timed_out["ACCEPTANCE_ELIGIBLE"])
        self.assertEqual(timed_out["RAW_RESULT"], "")
        self.assertEqual(timed_out["TEARDOWN_CHECK"]["status"], "BLOCKED")
        self.assertEqual(timed_out["TEARDOWN_CHECK"]["recovery_status"], "NOT_RUN")
        self.assertNotIn("reset", [command for command, _timeout in transport.commands])

    def test_timed_out_probe_envelope_names_its_last_step_marker(self):
        class HungProbeTransport(SimulatedPsplinkTransport):
            """A probe that durably wrote its progress, then never returned from its launch."""

            def __init__(self, hung_case, host0_log, **options):
                super().__init__(timeout_cases={hung_case}, **options)
                self.hung_case = hung_case
                self.host0_log = host0_log

            def run(self, command, timeout):
                if command == f"ldstart host0:/{self.hung_case}.prx":
                    _campaign_host0_log_path(self.host0_root, self.hung_case).write_text(
                        self.host0_log, encoding="utf-8"
                    )
                return super().run(command, timeout)

        progress = (
            "NAKAGAWA_PSP_STEP schema=1 case_id=hung-case step=open-registry\n"
            "NAKAGAWA_PSP_TEST schema=1 test_id=SYNTHETIC case_id=hung-case-open "
            "status=PASS result=0x1\n"
            "NAKAGAWA_PSP_STEP schema=1 case_id=hung-case step=bad-handle\n"
        )
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-hung-step-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id in ("transport-write", "hung-case"):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(case_id.encode())
                cases.append(CampaignCase(case_id, binary, 0.75))
            transport = HungProbeTransport("hung-case", progress)
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run(cases)

        hung = report["envelopes"][1]
        self.assertEqual(hung["PROCESS_STATUS"], "TIMEOUT")
        self.assertFalse(hung["ACCEPTANCE_ELIGIBLE"])
        self.assertEqual(hung["LAST_PROBE_STEP"], {"case_id": "hung-case", "step": "bad-handle"})
        self.assertIn("last step marker: hung-case/bad-handle",
                      " ".join(hung["QUALIFICATION_BLOCKERS"]))

    def test_failed_l0_shell_check_escalates_to_one_l1_restart(self):
        class ShellReturnsAfterRestartTransport(SimulatedPsplinkTransport):
            def restart(self):
                super().restart()
                self.fail_all_ver = False

        transport = ShellReturnsAfterRestartTransport(fail_all_ver=True)
        transport.case_started = True
        transport.current_case = "model-profile"
        transport._probe_loaded = True
        runner = PsplinkCampaignRunner(
            transport,
            console_model="PSP-3000-04g",
            source_commit=SOURCE_COMMIT,
            model_code=3,
        )

        self.assertTrue(runner._recover(transport._probe_uid, "unload confirmation lost"))
        self.assertIsNone(runner.terminal_reason)
        self.assertEqual(transport.restarts, 1)
        self.assertIn(
            "L1: restart owned usbhostfs_pc process and requalify",
            runner.recovery_events,
        )
        self.assertEqual(
            [command for command, _timeout in transport.commands].count(
                "modstun 0x04280001"
            ),
            1,
        )

    def test_19_real_adapter_attempts_l0_l1_l2_then_stops_at_physical_intervention(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-escalation-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            transport_write = scratch / "transport-write.prx"
            transport_write.write_bytes(b"synthetic transport PRX")
            binary = scratch / "model-profile.prx"
            binary.write_bytes(b"synthetic model-profile PRX")
            transport = SimulatedPsplinkTransport(
                fail_modstun=True,
                reset_timeout=True,
            )
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run([
                CampaignCase("transport-write", transport_write, 0.5),
                CampaignCase("model-profile", binary, 0.5),
            ])

        self.assertEqual(transport.restarts, 1)
        self.assertEqual(transport.transport_recoveries, 1)
        self.assertEqual(report["terminal_reason"], "PHYSICAL_INTERVENTION_REQUIRED")
        events = report["recovery_events"]
        recovery = " ".join(events)
        self.assertIn("shell verification attempt", recovery)
        ladder = [event.split(":", 1)[0] for event in events
                  if event.startswith(("L0:", "L1:", "L2:", "L4:"))]
        self.assertEqual(ladder, ["L0", "L1", "L2", "L2", "L4"])
        self.assertIn("L1: restart owned usbhostfs_pc process", recovery)
        self.assertIn("L2: re-attach PSPLink transport after PSPLink reset", recovery)
        self.assertIn("reset", [command for command, _ in transport.commands])
        self.assertEqual(
            [command for command, _ in transport.commands if command.startswith("ldstart ")],
            ["ldstart host0:/transport-write.prx", "ldstart host0:/model-profile.prx"],
        )

    @patch.object(run_psplink_module, "_read_hardware_lock", _lock_held)
    def test_20_process_transport_uses_argv_templates_timeout_and_owned_server_lifecycle(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-process-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            created = []
            popen_calls = []
            command_calls = []

            def popen_factory(command, **kwargs):
                popen_calls.append((command, kwargs))
                process = FakeUsbHostFsProcess(USBHOSTFS_BANNER)
                created.append(process)
                return process

            def command_runner(command, timeout):
                command_calls.append((command, timeout))
                if command == ["usbipd", "list"]:
                    created[-1].stdout.emit("Connected to device")
                    return 0, "2-1 054c:01c9 PSP Type B Attached\n", "", "PROCESS_EXITED"
                return 0, "ok", "", "PROCESS_EXITED"

            adapter = PsplinkProcessTransport(
                session_id="synthetic-session",
                pspsh_argv=["fake-pspsh", "-e", "{remote_command}"],
                usbhostfs_argv=["fake-usbhostfs", "{host0_root}", "{host0_root_wsl}"],
                host0_root=scratch,
                command_runner=command_runner,
                popen_factory=popen_factory,
            )
            adapter.start()
            adapter.run("ldstart host0:/probe.prx", 0.5)
            adapter.restart()
            adapter.stop()

        pspsh_calls = [call for call in command_calls if call[0][0] == "fake-pspsh"]
        self.assertEqual(pspsh_calls, [(["fake-pspsh", "-e", "ldstart host0:/probe.prx"], 0.5)])
        self.assertEqual(len(popen_calls), 2)
        self.assertEqual(popen_calls[0][0][0:2], ["fake-usbhostfs", str(scratch.resolve())])
        self.assertTrue(popen_calls[0][0][2].startswith("/"))
        self.assertEqual(popen_calls[0][1]["stdin"], subprocess.DEVNULL)
        self.assertEqual(popen_calls[0][1]["stdout"], subprocess.PIPE)
        self.assertEqual(popen_calls[0][1]["stderr"], subprocess.STDOUT)
        self.assertTrue(created[0].terminated)
        self.assertTrue(created[1].terminated)

    def test_21_nonfinite_timeout_is_rejected_before_transport_starts(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-unbounded-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            binary = scratch / "transport-write.prx"
            binary.write_bytes(b"synthetic PRX")
            transport = SimulatedPsplinkTransport()
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run([CampaignCase("transport-write", binary, float("inf"))])

        self.assertEqual(report["terminal_reason"], "INVALID_CASE_TIMEOUT")
        self.assertFalse(transport.started)

    def test_22_timeout_partial_bytes_are_decoded_as_nonsemantic_text(self):
        timeout = subprocess.TimeoutExpired(
            ["fake-pspsh"], 0.5, output=b"partial\xff", stderr=b"truncated"
        )
        with patch("psp_oracle.run_psplink.subprocess.run", side_effect=timeout):
            result = _run_command(["fake-pspsh"], 0.5)

        self.assertEqual(result, (None, "partial\ufffd", "truncated", "TIMEOUT"))

    def test_23_firmware_mismatch_stops_without_physical_intervention_claim(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-identity-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            binary = scratch / "transport-write.prx"
            binary.write_bytes(b"synthetic PRX")
            transport = SimulatedPsplinkTransport()
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
                expected_firmware="6.6.0",
            ).run([CampaignCase("transport-write", binary, 1.0)])

        self.assertEqual(report["terminal_reason"], "IDENTITY_MISMATCH")
        self.assertNotEqual(report["terminal_reason"], "PHYSICAL_INTERVENTION_REQUIRED")
        self.assertFalse(any(command == "reset" for command, _timeout in transport.commands))

    def test_24_qualified_cleanup_failure_uses_l2_reset_then_transport_reattach(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-ladder-order-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id in ("transport-write", "model-profile"):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(b"synthetic PRX")
                cases.append(CampaignCase(case_id, binary, 0.5))
            transport = SimulatedPsplinkTransport(fail_modstun=True, reset_timeout=True)
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run(cases)

        events = report["recovery_events"]
        self.assertEqual(report["terminal_reason"], "PHYSICAL_INTERVENTION_REQUIRED")
        host_restart = next(
            i for i, event in enumerate(events)
            if event.startswith("L1: restart owned usbhostfs_pc process")
        )
        reset = next(i for i, event in enumerate(events) if event.startswith("L2:"))
        reattach = next(
            i for i, event in enumerate(events)
            if event.startswith("L2: re-attach PSPLink transport after PSPLink reset")
        )
        self.assertLess(host_restart, reset)
        self.assertLess(reset, reattach)
        self.assertEqual(transport.transport_recoveries, 1)
        self.assertEqual(transport.restarts, 1)

    def test_missing_s1_snapshot_blocks_recovery_even_when_unload_fails(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-missing-s1-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id in ("transport-write", "model-profile"):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(b"synthetic PRX")
                cases.append(CampaignCase(case_id, binary, 0.02))
            transport = SimulatedPsplinkTransport(
                fail_modstun_cases={"model-profile"},
                fail_snapshot_call={"thlist": 5},
            )
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run(cases)

        envelope = report["envelopes"][1]
        self.assertEqual(envelope["TEARDOWN_CHECK"]["status"], "BLOCKED")
        self.assertEqual(envelope["TEARDOWN_CHECK"]["recovery_status"], "NOT_RUN")
        self.assertNotIn("reset", [command for command, _timeout in transport.commands])
        self.assertEqual(transport.restarts, 0)

    def test_host0_capture_timeout_blocks_recovery_even_when_unload_fails(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-missing-capture-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            binary = scratch / "transport-write.prx"
            binary.write_bytes(b"synthetic PRX")
            transport = SimulatedPsplinkTransport(
                fail_modstun_cases={"transport-write"}, write_host0_logs=False
            )
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run([CampaignCase("transport-write", binary, 0.01)])

        envelope = report["envelopes"][0]
        self.assertEqual(envelope["TEARDOWN_CHECK"]["status"], "BLOCKED")
        self.assertEqual(envelope["TEARDOWN_CHECK"]["recovery_status"], "NOT_RUN")
        self.assertIn(
            "reached no completion marker", " ".join(envelope["TEARDOWN_CHECK"]["issues"])
        )
        self.assertNotIn("reset", [command for command, _timeout in transport.commands])
        self.assertEqual(transport.restarts, 0)

    def test_failed_case_with_cleanup_failure_does_not_reset(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        failed_log = (
            CAMPAIGN_META
            + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-SYSTEM-001 "
            "case_id=model-profile status=FAIL result=0x3 "
            "out0=0x3 out1=0x06060110 out2=0xde\n"
            "NAKAGAWA_PSP_COMPLETE schema=1 status=PASS\n"
        )
        with tempfile.TemporaryDirectory(prefix="runner-failed-case-cleanup-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id in ("transport-write", "model-profile"):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(b"synthetic PRX")
                cases.append(CampaignCase(case_id, binary, 0.02))
            transport = SimulatedPsplinkTransport(
                fail_modstun_cases={"model-profile"},
                host0_log_contents={"model-profile": failed_log},
            )
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run(cases)

        envelope = report["envelopes"][1]
        self.assertFalse(envelope["ACCEPTANCE_ELIGIBLE"])
        self.assertEqual(envelope["TEARDOWN_CHECK"]["recovery_status"], "NOT_RUN")
        self.assertNotIn("reset", [command for command, _timeout in transport.commands])
        self.assertEqual(transport.restarts, 0)

    def test_second_case_teardown_failure_cannot_issue_a_second_l2_reset(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-reset-once-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id in ("transport-write", "model-profile"):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(b"synthetic PRX")
                cases.append(CampaignCase(case_id, binary, 0.02))
            transport = SimulatedPsplinkTransport(
                fail_modstun_cases={"transport-write", "model-profile"}
            )
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run(cases)

        self.assertEqual(
            [command for command, _timeout in transport.commands].count("reset"), 1
        )
        self.assertEqual(
            sum(event.startswith("L2: reset ") for event in report["recovery_events"]), 1
        )
        self.assertEqual(report["terminal_reason"], "PHYSICAL_INTERVENTION_REQUIRED")

    def test_exprint_observation_is_diagnostic_and_not_a_pass_gate(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-exprint-diagnostic-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            binary = scratch / "transport-write.prx"
            binary.write_bytes(b"synthetic PRX")
            transport = SimulatedPsplinkTransport()
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run([CampaignCase("transport-write", binary, 0.02)])

        teardown = report["envelopes"][0]["TEARDOWN_CHECK"]
        self.assertEqual(teardown["status"], "PASS")
        self.assertEqual(teardown["exprint_status"], "NOT_RUN")
        self.assertEqual(teardown["exprint_command_status"], "PROCESS_EXITED")

    def test_l1_requalification_failure_proceeds_to_bounded_l2(self):
        class FailPspverOnceAfterRestartTransport(SimulatedPsplinkTransport):
            fail_pspver_after_restart = False

            def restart(self):
                super().restart()
                self.fail_pspver_after_restart = True

            def run(self, command, timeout):
                if command == "pspver" and self.fail_pspver_after_restart:
                    self.fail_pspver_after_restart = False
                    self.commands.append((command, timeout))
                    return None, "", "", "TIMEOUT"
                return super().run(command, timeout)

        transport = FailPspverOnceAfterRestartTransport(fail_modstun=True)
        transport.case_started = True
        transport.current_case = "model-profile"
        transport._probe_loaded = True
        runner = PsplinkCampaignRunner(
            transport,
            console_model="PSP-3000-04g",
            source_commit=SOURCE_COMMIT,
            model_code=3,
        )

        self.assertTrue(runner._recover(transport._probe_uid, "confirmed unload failure"))
        ladder = [event.split(":", 1)[0] for event in runner.recovery_events
                  if event.startswith(("L0:", "L1:", "L2:", "L4:"))]
        self.assertEqual(ladder, ["L0", "L1", "L2", "L2", "L2"])
        events = runner.recovery_events
        self.assertLess(
            next(i for i, event in enumerate(events) if event.startswith("L2: reset ")),
            next(i for i, event in enumerate(events) if event.startswith("L2: re-attach ")),
        )
        self.assertEqual(
            [command for command, _timeout in transport.commands].count("reset"), 1
        )

    def test_l1_requalification_exception_proceeds_to_bounded_l2(self):
        transport = SimulatedPsplinkTransport(fail_modstun=True)
        transport.case_started = True
        transport.current_case = "model-profile"
        transport._probe_loaded = True
        runner = PsplinkCampaignRunner(
            transport,
            console_model="PSP-3000-04g",
            source_commit=SOURCE_COMMIT,
            model_code=3,
        )
        original_qualify = runner._qualify
        raised = False

        def fail_once_after_restart():
            nonlocal raised
            if transport.restarts and not raised:
                raised = True
                raise RuntimeError("synthetic post-restart qualification failure")
            return original_qualify()

        with patch.object(runner, "_qualify", side_effect=fail_once_after_restart):
            self.assertTrue(runner._recover(
                transport._probe_uid, "confirmed unload failure"
            ))

        self.assertEqual(
            [command for command, _timeout in transport.commands].count("reset"), 1
        )
        self.assertTrue(any(
            "L1: post-restart qualification raised RuntimeError" in event
            for event in runner.recovery_events
        ))
        self.assertTrue(any(event.startswith("L2: reset ") for event in runner.recovery_events))

    def test_l1_unload_status_exception_proceeds_to_bounded_l2(self):
        transport = SimulatedPsplinkTransport(fail_modstun=True)
        transport.case_started = True
        transport.current_case = "model-profile"
        transport._probe_loaded = True
        runner = PsplinkCampaignRunner(
            transport,
            console_model="PSP-3000-04g",
            source_commit=SOURCE_COMMIT,
            model_code=3,
        )
        original_unload_status = runner._unload_status
        raised = False

        def fail_once_after_restart(module_uid):
            nonlocal raised
            if transport.restarts and not raised:
                raised = True
                raise RuntimeError("synthetic post-restart unload verification failure")
            return original_unload_status(module_uid)

        with patch.object(runner, "_unload_status", side_effect=fail_once_after_restart):
            self.assertTrue(runner._recover(
                transport._probe_uid, "confirmed unload failure"
            ))

        self.assertEqual(
            [command for command, _timeout in transport.commands].count("reset"), 1
        )
        self.assertTrue(any(
            "L1: unload verification raised RuntimeError" in event
            for event in runner.recovery_events
        ))
        self.assertTrue(any(event.startswith("L2: reset ") for event in runner.recovery_events))

    def test_l0_shell_qualification_exception_proceeds_to_bounded_recovery(self):
        transport = SimulatedPsplinkTransport(fail_modstun=True)
        transport.case_started = True
        transport.current_case = "model-profile"
        transport._probe_loaded = True
        runner = PsplinkCampaignRunner(
            transport,
            console_model="PSP-3000-04g",
            source_commit=SOURCE_COMMIT,
            model_code=3,
        )
        original_shell_qualified = runner._shell_qualified
        raised = False

        def fail_once():
            nonlocal raised
            if not raised:
                raised = True
                raise RuntimeError("synthetic L0 shell qualification failure")
            return original_shell_qualified()

        with patch.object(runner, "_shell_qualified", side_effect=fail_once):
            self.assertTrue(runner._recover(
                transport._probe_uid, "confirmed unload failure"
            ))

        self.assertTrue(any(
            "L0: shell qualification raised RuntimeError" in event
            for event in runner.recovery_events
        ))
        self.assertEqual(
            [command for command, _timeout in transport.commands].count("reset"), 1
        )

    def test_l0_unload_status_exception_proceeds_to_bounded_recovery(self):
        transport = SimulatedPsplinkTransport(fail_modstun=True)
        transport.case_started = True
        transport.current_case = "model-profile"
        transport._probe_loaded = True
        runner = PsplinkCampaignRunner(
            transport,
            console_model="PSP-3000-04g",
            source_commit=SOURCE_COMMIT,
            model_code=3,
        )
        original_unload_status = runner._unload_status
        raised = False

        def fail_once(module_uid):
            nonlocal raised
            if not raised:
                raised = True
                raise RuntimeError("synthetic L0 unload verification failure")
            return original_unload_status(module_uid)

        with patch.object(runner, "_unload_status", side_effect=fail_once):
            self.assertTrue(runner._recover(
                transport._probe_uid, "confirmed unload failure"
            ))

        self.assertTrue(any(
            "L0: unload verification raised RuntimeError" in event
            for event in runner.recovery_events
        ))
        self.assertEqual(
            [command for command, _timeout in transport.commands].count("reset"), 1
        )

    def test_l2_shell_qualification_exception_ends_at_l4_without_reset(self):
        transport = SimulatedPsplinkTransport()
        runner = PsplinkCampaignRunner(
            transport,
            console_model="PSP-3000-04g",
            source_commit=SOURCE_COMMIT,
            model_code=3,
        )
        with patch.object(
            runner, "_shell_qualified",
            side_effect=RuntimeError("synthetic L2 shell qualification failure"),
        ):
            self.assertFalse(runner._reset_once("test L2 shell failure"))

        self.assertEqual(runner.terminal_reason, "PHYSICAL_INTERVENTION_REQUIRED")
        self.assertNotIn("reset", [command for command, _timeout in transport.commands])
        self.assertTrue(any(event.startswith("L4:") for event in runner.recovery_events))

    def test_l2_reset_command_exception_ends_at_l4_without_reattach(self):
        class ResetCommandFailureTransport(SimulatedPsplinkTransport):
            def run(self, command, timeout):
                if command == "reset":
                    self.commands.append((command, timeout))
                    raise RuntimeError("synthetic L2 reset command failure")
                return super().run(command, timeout)

        transport = ResetCommandFailureTransport()
        runner = PsplinkCampaignRunner(
            transport,
            console_model="PSP-3000-04g",
            source_commit=SOURCE_COMMIT,
            model_code=3,
        )
        self.assertFalse(runner._reset_once("test L2 reset failure"))

        self.assertEqual(runner.terminal_reason, "PHYSICAL_INTERVENTION_REQUIRED")
        self.assertEqual(
            [command for command, _timeout in transport.commands].count("reset"), 1
        )
        self.assertEqual(transport.transport_recoveries, 0)
        self.assertTrue(any(event.startswith("L4:") for event in runner.recovery_events))

    def test_l2_post_reset_qualification_exception_ends_at_l4(self):
        transport = SimulatedPsplinkTransport()
        runner = PsplinkCampaignRunner(
            transport,
            console_model="PSP-3000-04g",
            source_commit=SOURCE_COMMIT,
            model_code=3,
        )
        with patch.object(
            runner, "_qualify",
            side_effect=RuntimeError("synthetic L2 post-reset qualification failure"),
        ):
            self.assertFalse(runner._reset_once("test L2 qualification failure"))

        self.assertEqual(runner.terminal_reason, "PHYSICAL_INTERVENTION_REQUIRED")
        self.assertEqual(
            [command for command, _timeout in transport.commands].count("reset"), 1
        )
        self.assertEqual(transport.transport_recoveries, 1)
        self.assertTrue(any(
            "L2: post-reset qualification raised RuntimeError" in event
            for event in runner.recovery_events
        ))

    def test_l1_restart_failure_continues_to_bounded_l2_reset(self):
        class RestartFailureTransport(SimulatedPsplinkTransport):
            def restart(self):
                raise OSError("synthetic restart failure")

        transport = RestartFailureTransport(fail_modstun=True)
        transport.case_started = True
        transport.current_case = "model-profile"
        transport._probe_loaded = True
        runner = PsplinkCampaignRunner(
            transport,
            console_model="PSP-3000-04g",
            source_commit=SOURCE_COMMIT,
            model_code=3,
        )

        self.assertTrue(runner._recover(transport._probe_uid, "confirmed unload failure"))
        self.assertIsNone(runner.terminal_reason)
        self.assertEqual(
            [command for command, _timeout in transport.commands].count("reset"), 1
        )
        self.assertTrue(any(
            "L1: host stack restart raised OSError; continue to L2" in event
            for event in runner.recovery_events
        ))
        self.assertTrue(any(event.startswith("L2: reset ") for event in runner.recovery_events))

    def test_l1_transport_reattach_failure_continues_to_one_l2_reset(self):
        class OneFailedL1ReattachTransport(SimulatedPsplinkTransport):
            def __init__(self):
                super().__init__(fail_modstun=True)
                self.reattach_attempts = 0

            def recover_psplink_transport(self, *args, **kwargs):
                self.reattach_attempts += 1
                if self.reattach_attempts == 1:
                    return False, "synthetic L1 re-attach failure", None
                return super().recover_psplink_transport(*args, **kwargs)

        transport = OneFailedL1ReattachTransport()
        transport.case_started = True
        transport.current_case = "model-profile"
        transport._probe_loaded = True
        transport.waiting_for_device = True
        runner = PsplinkCampaignRunner(
            transport,
            console_model="PSP-3000-04g",
            source_commit=SOURCE_COMMIT,
            model_code=3,
        )

        self.assertTrue(runner._recover(transport._probe_uid, "confirmed unload failure"))
        self.assertIsNone(runner.terminal_reason)
        self.assertEqual(transport.reattach_attempts, 2)
        self.assertEqual(
            [command for command, _timeout in transport.commands].count("reset"), 1
        )
        self.assertTrue(any(
            "L1: synthetic L1 re-attach failure; continue to L2 reset" in event
            for event in runner.recovery_events
        ))
        self.assertTrue(any(event.startswith("L2: reset ") for event in runner.recovery_events))
        self.assertFalse(any(event.startswith("L4:") for event in runner.recovery_events))

    def test_exhausted_l1_reattach_is_reported_when_l2_lacks_qualified_shell(self):
        transport = SimulatedPsplinkTransport()
        transport.waiting_for_device = True
        runner = PsplinkCampaignRunner(
            transport,
            console_model="PSP-3000-04g",
            source_commit=SOURCE_COMMIT,
            model_code=3,
        )
        # Model the L1 re-attach budget already consumed during the preceding
        # recovery attempt. L2 is not authorized to issue reset without a
        # currently qualified PSPLink shell.
        runner._l1_transport_reattach_attempted = True

        self.assertFalse(runner._reset_once("test exhausted L1 re-attach"))

        self.assertEqual(
            [command for command, _timeout in transport.commands].count("reset"), 0
        )
        self.assertEqual(runner.terminal_reason, "PHYSICAL_INTERVENTION_REQUIRED")
        self.assertTrue(any(
            "L1 PSPLink transport re-attach limit exhausted" in event
            for event in runner.recovery_events
        ))
        self.assertTrue(any(
            "reset not attempted because PSPLink shell qualification failed" in event
            for event in runner.recovery_events
        ), runner.recovery_events)

    def test_25_waiting_for_device_runs_one_transport_reattach_and_returns_verified_ver(self):
        transport = SimulatedPsplinkTransport()
        runner = PsplinkCampaignRunner(
            transport,
            console_model="PSP-3000-04g",
            source_commit=SOURCE_COMMIT,
        )
        transport.waiting_for_device = True

        result = runner._request("ver")

        self.assertEqual(result, (0, "PSPLink v3.2.1\n", "", "PROCESS_EXITED"))
        self.assertEqual(transport.transport_recoveries, 1)
        self.assertIn("USBHostFS reported `waiting for device`", " ".join(runner.recovery_events))

    def test_shell_qualification_retries_one_lost_ver_and_campaign_passes(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-ver-retry-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            binary = scratch / "transport-write.prx"
            binary.write_bytes(b"synthetic PRX")
            transport = SimulatedPsplinkTransport(fail_first_ver_once=True)
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run([CampaignCase("transport-write", binary, 1.0)])

        ver_events = [
            event for event in report["recovery_events"]
            if event.startswith("shell verification attempt ")
        ]
        self.assertIsNone(report["terminal_reason"])
        self.assertEqual(len(ver_events), 3)
        self.assertIn("reply timed out or was lost", ver_events[0])
        self.assertIn("PASS", ver_events[1])
        self.assertIn("PASS", ver_events[2])
        self.assertTrue(report["envelopes"][0]["ACCEPTANCE_ELIGIBLE"])

    def test_initial_shell_qualification_exhaustion_is_a_transport_start_failure(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-ver-exhausted-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            binary = scratch / "transport-write.prx"
            binary.write_bytes(b"synthetic PRX")
            transport = SimulatedPsplinkTransport(fail_all_ver=True)
            transport.host0_root = scratch
            runner = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                shell_verification_timeout=1.0,
            )
            report = runner.run([CampaignCase("transport-write", binary, 1.0)])

        attempts = [
            event for event in report["recovery_events"]
            if event.startswith("shell verification attempt ")
        ]
        commands = [command for command, _timeout in transport.commands]
        # Nothing was launched, so the stop is a transport start failure and
        # never a power-cycle (physical intervention) demand.
        self.assertEqual(report["terminal_reason"], "TRANSPORT_START_FAILED")
        self.assertEqual(report["state"], "STOPPED")
        self.assertIsNone(report["resume_case_index"])
        self.assertEqual(len(attempts), 3)
        self.assertIn("usbipd list", report["recovery_events"][-1])
        self.assertIn("pspsh -e ver", report["recovery_events"][-1])
        self.assertIn("no power cycle is required", report["recovery_events"][-1])
        self.assertIn("pspsh -e ver", report["transport_start_problem"])
        self.assertFalse(any(command.startswith(("ldstart", "reset")) for command in commands))
        self.assertEqual(report["envelopes"], [])
        self.assertTrue(transport.stopped)

    def test_transport_start_failure_stops_before_any_psplink_command(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-start-fail-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            binary = scratch / "transport-write.prx"
            binary.write_bytes(b"synthetic PRX")
            transport = SimulatedPsplinkTransport(
                start_error="USBHostFS did not report `Connected to device`"
            )
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
            ).run([CampaignCase("transport-write", binary, 1.0)])

        self.assertEqual(report["terminal_reason"], "TRANSPORT_START_FAILED")
        self.assertEqual(report["state"], "STOPPED")
        self.assertEqual(transport.commands, [])
        self.assertTrue(transport.stopped)
        self.assertEqual(
            report["transport_start_problem"],
            "USBHostFS did not report `Connected to device`",
        )
        self.assertIn("no power cycle is required", report["recovery_events"][-1])

    def test_campaign_unknown_model_is_captured_but_not_acceptance_eligible(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-unknown-model-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            binary = scratch / "transport-write.prx"
            binary.write_bytes(b"synthetic PRX")
            transport = SimulatedPsplinkTransport()
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="unknown",
                source_commit=SOURCE_COMMIT,
            ).run([CampaignCase("transport-write", binary, 1.0)])

        envelope = report["envelopes"][0]
        self.assertIn("status=PASS", envelope["RAW_RESULT"])
        self.assertFalse(envelope["ACCEPTANCE_ELIGIBLE"])
        self.assertTrue(any("model is the fixture placeholder 'unknown'" in blocker
                            for blocker in envelope["ACCEPTANCE_BLOCKERS"]))

    def test_unmeasured_labels_are_placeholders_not_identity(self):
        from psp_oracle.protocol import is_unmeasured

        for label in ("unmeasured", "Unmeasured", "not-measured", "N/A", "un measured",
                      "unverified", "TBD", "?", "-", " "):
            with self.subTest(label=label):
                self.assertTrue(is_unmeasured(label))
        for label in ("PSP-3000", "04g", "6.61", "psp3000-ark5"):
            with self.subTest(label=label):
                self.assertFalse(is_unmeasured(label))

    def test_session_qualification_reflects_the_session_not_the_case_phase(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        for failing, expected in ((False, "QUALIFIED"), (True, "QUALIFIED")):
            with self.subTest(failing=failing), tempfile.TemporaryDirectory(
                prefix="runner-session-status-", dir=fixture_dir
            ) as scratch_name:
                scratch = Path(scratch_name)
                binary = scratch / "transport-write.prx"
                binary.write_bytes(b"synthetic PRX")
                transport = SimulatedPsplinkTransport(
                    timeout_cases={"transport-write"} if failing else None
                )
                transport.host0_root = scratch
                report = PsplinkCampaignRunner(
                    transport,
                    console_model="PSP-3000",
                    source_commit=SOURCE_COMMIT,
                ).run([CampaignCase("transport-write", binary, 1.0)])
                envelope = report["envelopes"][0]
                self.assertEqual(envelope["SESSION_QUALIFICATION_STATUS"], expected)
                if not failing:
                    self.assertEqual(envelope["QUALIFICATION_STATUS"], "QUALIFIED")
                else:
                    self.assertEqual(envelope["PROCESS_STATUS"], "TIMEOUT")
                    self.assertEqual(envelope["QUALIFICATION_STATUS"], "UNQUALIFIED")
                    self.assertFalse(envelope["ACCEPTANCE_ELIGIBLE"])

    def test_campaign_unmeasured_model_is_not_acceptance_eligible(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-unmeasured-model-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            binary = scratch / "transport-write.prx"
            binary.write_bytes(b"synthetic PRX")
            transport = SimulatedPsplinkTransport()
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="unmeasured",
                source_commit=SOURCE_COMMIT,
            ).run([CampaignCase("transport-write", binary, 1.0)])

        envelope = report["envelopes"][0]
        self.assertFalse(envelope["ACCEPTANCE_ELIGIBLE"])
        self.assertTrue(any("model is the fixture placeholder 'unmeasured'" in blocker
                            for blocker in envelope["ACCEPTANCE_BLOCKERS"]))

    def test_shell_verification_respects_total_deadline_and_per_attempt_cap(self):
        now = [0.0]
        timeouts = []
        events = []

        def run(command, timeout):
            self.assertEqual(command, "ver")
            timeouts.append(timeout)
            now[0] += timeout
            return None, "", "", "TIMEOUT"

        verified, result, attempts, detail = _verify_psplink_shell(
            run,
            20.0,
            record_event=events.append,
            clock=lambda: now[0],
        )

        self.assertFalse(verified)
        self.assertEqual(result[3], "TIMEOUT")
        self.assertEqual(attempts, 2)
        self.assertEqual(timeouts, [15.0, 5.0])
        self.assertLessEqual(sum(timeouts), 20.0)
        self.assertEqual(len(events), 2)
        self.assertIn("exhausted 2 of 3", detail)

    def test_shell_verification_retries_unknown_command_transport_output(self):
        transport = SimulatedPsplinkTransport(unknown_command_ver_once=True)
        events = []

        verified, result, attempts, _detail = _verify_psplink_shell(
            transport.run,
            45.0,
            take_unknown_command_events=transport.take_unknown_command_events,
            record_event=events.append,
        )

        self.assertTrue(verified)
        self.assertEqual(attempts, 2)
        self.assertIn("Error, unknown command", events[0])
        self.assertIn("PSPLink", result[1])

    def test_reset_nonzero_exit_recovers_only_after_transport_and_shell_qualification(self):
        transport = SimulatedPsplinkTransport(reset_returncode=1)
        runner = PsplinkCampaignRunner(
            transport,
            console_model="PSP-3000-04g",
            source_commit=SOURCE_COMMIT,
        )

        self.assertTrue(runner._reset_once("simulated reset"))

        self.assertIsNone(runner.terminal_reason)
        self.assertIn("reset process exited 1", " ".join(runner.recovery_events))
        self.assertEqual(transport.transport_recoveries, 1)

    def test_waiting_for_device_discards_an_inflight_probe_result(self):
        transport = SimulatedPsplinkTransport()
        runner = PsplinkCampaignRunner(
            transport,
            console_model="PSP-3000-04g",
            source_commit=SOURCE_COMMIT,
        )
        transport.waiting_for_device = True

        result = runner._request("ldstart host0:/probe.prx")

        self.assertEqual(result[3], "TRANSPORT_RECOVERED")
        self.assertEqual(runner.terminal_reason, "TRANSPORT_RESULT_DISCARDED")
        self.assertEqual(transport.transport_recoveries, 1)

    def test_26_usbipd_parser_uses_dynamic_psplink_busid_and_ignores_other_devices(self):
        output = (
            "Connected:\n"
            "BUSID  VID:PID    DEVICE                         STATE\n"
            "1-3    1234:5678  Unrelated adapter              Shared\n"
            "9-7.2  054c:01c9  PSP Type B                    Shared\n"
        )

        self.assertEqual(_parse_usbipd_psplink_devices(output), [("9-7.2", "Shared")])

    @patch.object(run_psplink_module, "_read_hardware_lock", _lock_held)
    def test_27_shared_psplink_device_is_attached_and_verified(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="usbipd-reattach-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            process = FakeUsbHostFsProcess(USBHOSTFS_BANNER)
            calls = []
            shell_events = []
            ver_attempts = 0
            usbipd_states = ["Attached", "Shared"]
            adapter = None

            def command_runner(command, timeout):
                calls.append(command)
                if command == ["usbipd", "list"]:
                    state = usbipd_states.pop(0)
                    if state == "Attached":
                        process.stdout.emit("Connected to device")
                    return (
                        0,
                        "Connected:\nBUSID VID:PID DEVICE STATE\n"
                        f"9-7.2 054c:01c9 PSP Type B {state}\n",
                        "",
                        "PROCESS_EXITED",
                    )
                if command == ["usbipd", "attach", "--wsl", "--busid", "9-7.2"]:
                    adapter._observe_server_output("Connected to device")
                    return 0, "attached", "", "PROCESS_EXITED"
                if command == ["fake-pspsh", "-e", "ver"]:
                    nonlocal ver_attempts
                    ver_attempts += 1
                    if ver_attempts == 1:
                        adapter._observe_server_output("Error, unknown command 00000000")
                        return None, "", "", "TIMEOUT"
                    return 0, "PSPLink v3.2.1\n", "", "PROCESS_EXITED"
                raise AssertionError(f"unexpected command: {command}")

            adapter = PsplinkProcessTransport(
                session_id="synthetic-session",
                pspsh_argv=["fake-pspsh", "-e", "{remote_command}"],
                usbhostfs_argv=["fake-usbhostfs", "{host0_root}"],
                host0_root=scratch,
                command_runner=command_runner,
                popen_factory=lambda _command, **_kwargs: process,
            )
            adapter.start()
            self.assertFalse(adapter.take_waiting_for_device())
            adapter._observe_server_output("Read cancelled (remote disconnected)")
            adapter._observe_server_output("waiting for device...")
            self.assertTrue(adapter.take_waiting_for_device())

            recovered, detail, verification = adapter.recover_psplink_transport(
                0.5,
                shell_verification_timeout=1.0,
                record_event=shell_events.append,
            )
            adapter.stop()

        self.assertTrue(recovered)
        self.assertIn("9-7.2", detail)
        self.assertEqual(verification, (0, "PSPLink v3.2.1\n", "", "PROCESS_EXITED"))
        self.assertEqual(ver_attempts, 2)
        self.assertIn("Error, unknown command", shell_events[0])
        self.assertIn("PASS", shell_events[1])
        self.assertEqual(calls, [
            ["usbipd", "list"],
            ["usbipd", "list"],
            ["usbipd", "attach", "--wsl", "--busid", "9-7.2"],
            ["fake-pspsh", "-e", "ver"],
            ["fake-pspsh", "-e", "ver"],
        ])

    def test_28_transport_reattach_failure_stops_after_one_attach_attempt(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="usbipd-reattach-fail-", dir=fixture_dir) as scratch_name:
            calls = []

            def command_runner(command, timeout):
                calls.append(command)
                if command == ["usbipd", "list"]:
                    return 0, "2-4 054c:01c9 PSP Type B Shared\n", "", "PROCESS_EXITED"
                return 1, "attach failed", "", "PROCESS_EXITED"

            adapter = PsplinkProcessTransport(
                session_id="synthetic-session",
                pspsh_argv=["fake-pspsh"],
                usbhostfs_argv=["fake-usbhostfs"],
                host0_root=Path(scratch_name),
                command_runner=command_runner,
            )
            recovered, detail, verification = adapter.recover_psplink_transport(0.1)

        self.assertFalse(recovered)
        self.assertIn("usbipd attach failed", detail)
        self.assertIn("usbipd attach --wsl --busid 2-4", detail)
        self.assertIsNone(verification)
        self.assertEqual(calls, [
            ["usbipd", "list"],
            ["usbipd", "attach", "--wsl", "--busid", "2-4"],
        ])
        self.assertFalse(any("bind" in " ".join(command) for command in calls))

    def test_29_absent_psplink_device_reports_manual_recovery_without_attach(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="usbipd-absent-", dir=fixture_dir) as scratch_name:
            calls = []

            def command_runner(command, timeout):
                calls.append(command)
                return 0, "1-1 1234:5678 Other USB device Shared\n", "", "PROCESS_EXITED"

            adapter = PsplinkProcessTransport(
                session_id="synthetic-session",
                pspsh_argv=["fake-pspsh"],
                usbhostfs_argv=["fake-usbhostfs"],
                host0_root=Path(scratch_name),
                command_runner=command_runner,
            )
            recovered, detail, verification = adapter.recover_psplink_transport(0.1)

        self.assertFalse(recovered)
        self.assertIn("054c:01c9 is absent from usbipd list", detail)
        self.assertIn("usbipd list", detail)
        self.assertIsNone(verification)
        self.assertEqual(calls, [["usbipd", "list"]])

    def test_30_unbound_psplink_device_reports_manual_bind_without_running_it(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="usbipd-unbound-", dir=fixture_dir) as scratch_name:
            calls = []

            def command_runner(command, timeout):
                calls.append(command)
                return 0, "4-2 054c:01c9 PSP Type B Not shared\n", "", "PROCESS_EXITED"

            adapter = PsplinkProcessTransport(
                session_id="synthetic-session",
                pspsh_argv=["fake-pspsh"],
                usbhostfs_argv=["fake-usbhostfs"],
                host0_root=Path(scratch_name),
                command_runner=command_runner,
            )
            recovered, detail, verification = adapter.recover_psplink_transport(0.1)

        self.assertFalse(recovered)
        self.assertIn("not bound", detail)
        self.assertIn("usbipd bind --busid 4-2", detail)
        self.assertIn("usbipd attach --wsl --busid 4-2", detail)
        self.assertIsNone(verification)
        self.assertEqual(calls, [["usbipd", "list"]])


    def test_fixed_campaign_contracts_reject_truncation_and_wrong_fields(self):
        for case_id, (test_id, contract) in _FIXED_CAMPAIGN_CASES.items():
            rows = []
            for row_id, out_count, statuses in contract:
                status = sorted(statuses)[0]
                fields = " ".join(
                    f"out{index}=0x{index + 1:08x}" for index in range(out_count)
                )
                rows.append(
                    f"NAKAGAWA_PSP_TEST schema=1 test_id={test_id} "
                    f"case_id={row_id} status={status} result=0x00000000 {fields}\n"
                )
            stream = CAMPAIGN_META + "".join(rows)
            self.assertTrue(
                run_psplink_module._campaign_stream_complete(stream, case_id),
                case_id,
            )
            with self.assertRaises(PspProtocolError, msg=case_id):
                _parse_campaign_records(CAMPAIGN_META + "".join(rows[:-1]), case_id)
            malformed = stream.replace(" out0=0x00000001", "", 1)
            with self.assertRaises(PspProtocolError, msg=case_id):
                _parse_campaign_records(malformed, case_id)

    def test_campaign_queue_dry_run_validates_all_staged_cases_without_launching(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(
            prefix="campaign-plan-dry-run-", dir=fixture_dir
        ) as scratch_name:
            scratch = Path(scratch_name)
            host0_root = scratch / "host0"
            output_root = scratch / "output"
            host0_root.mkdir()
            output_root.mkdir()
            cases = []
            for case_id in CAMPAIGN_QUEUE_CASES:
                (host0_root / f"{case_id}.prx").write_bytes(b"synthetic PRX")
                cases.append({
                    "case_id": case_id,
                    "prx": f"{case_id}.prx",
                    "timeout_seconds": 30,
                })
            plan_path = scratch / "campaign-plan.json"
            plan_path.write_text(json.dumps({
                "schema": 1,
                "campaign_id": "synthetic-campaign",
                "session_id": "synthetic-session",
                "source_commit": SOURCE_COMMIT,
                "console_model": "PSP-3000",
                "host0_root": "host0",
                "report_path": "output/report.json",
                "checkpoint_path": "output/checkpoint.json",
                "cases": cases,
            }), encoding="utf-8")
            with patch.object(run_psplink_module, "_check_source_tree", return_value=None):
                code, report = run_campaign_plan(
                    plan_path,
                    dry_run=True,
                    confirm_power_cycle=False,
                    pspsh_argv=["pspsh", "-e", "{remote_command}"],
                    usbhostfs_argv=["usbhostfs_pc", "{host0_root}"],
                )

            self.assertEqual(code, 0)
            self.assertEqual(report["status"], "VALIDATED_OFFLINE")
            self.assertFalse(report["hardware_started"])
            self.assertEqual(report["case_count"], len(CAMPAIGN_QUEUE_CASES))
            self.assertEqual(
                report["estimated_total_seconds"],
                _campaign_queue_summary()["estimated_total_seconds"],
            )
            self.assertFalse((output_root / "checkpoint.json").exists())

    def test_campaign_resets_between_cases_and_reuses_preflight_qualification(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(
            prefix="campaign-reset-between-", dir=fixture_dir
        ) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id in ("transport-write", "model-profile"):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(b"synthetic PRX")
                cases.append(CampaignCase(case_id, binary, 1.0))
            transport = SimulatedPsplinkTransport(
                transport_file_cases={"transport-write"}
            )
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run(cases, reset_between_cases=True, stop_on_incomplete=True)

        commands = [command for command, _timeout in transport.commands]
        self.assertIsNone(report["terminal_reason"])
        self.assertEqual(commands.count("reset"), 1)
        self.assertEqual(sum(command.startswith("ldstart ") for command in commands), 2)
        self.assertTrue(report["envelopes"][0]["ACCEPTANCE_ELIGIBLE"])
        self.assertTrue(report["envelopes"][1]["ACCEPTANCE_ELIGIBLE"])
        self.assertTrue(transport.stopped)

    def test_campaign_timeout_stops_without_cleanup_or_automatic_reset(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(
            prefix="campaign-timeout-stop-", dir=fixture_dir
        ) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id in ("transport-write", "model-profile", "smoke"):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(b"synthetic PRX")
                cases.append(CampaignCase(case_id, binary, 0.01))
            transport = SimulatedPsplinkTransport(
                timeout_cases={"model-profile"},
                transport_file_cases={"transport-write"},
            )
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run(cases, reset_between_cases=True, stop_on_incomplete=True)

        commands = [command for command, _timeout in transport.commands]
        second_launch = commands.index("ldstart host0:/model-profile.prx")
        self.assertEqual(report["terminal_reason"], "PHYSICAL_INTERVENTION_REQUIRED")
        self.assertEqual(report["intervention_case_id"], "model-profile")
        self.assertEqual(report["resume_case_index"], 2)
        self.assertEqual(commands.count("ldstart host0:/smoke.prx"), 0)
        self.assertNotIn("reset", commands[second_launch + 1:])
        self.assertNotIn("modstun 0x04280001", commands[second_launch + 1:])


@patch.object(run_psplink_module, "_read_hardware_lock", _lock_held)
class TransportStartReadinessTests(unittest.TestCase):
    """PSPLink start-up waits for a positive USB link signal (no fixed sleeps)."""

    def _adapter(self, scratch: Path, process, command_runner, *, start_timeout=5.0):
        return PsplinkProcessTransport(
            session_id="synthetic-session",
            pspsh_argv=["fake-pspsh", "-e", "{remote_command}"],
            usbhostfs_argv=["fake-usbhostfs", "{host0_root}"],
            host0_root=scratch,
            command_runner=command_runner,
            popen_factory=lambda _command, **_kwargs: process,
            start_timeout=start_timeout,
        )

    @staticmethod
    def _usbipd_row(state: str) -> tuple[int, str, str, str]:
        return 0, f"BUSID VID:PID DEVICE STATE\n2-1 054c:01c9 PSP Type B {state}\n", "", "PROCESS_EXITED"

    def test_shared_device_is_attached_and_start_waits_for_connected_to_device(self):
        process = FakeUsbHostFsProcess(USBHOSTFS_BANNER)
        calls = []

        def command_runner(command, timeout):
            calls.append(command)
            if command == ["usbipd", "list"]:
                return self._usbipd_row("Shared")
            if command == ["usbipd", "attach", "--wsl", "--busid", "2-1"]:
                process.stdout.emit("Connected to device")
                return 0, "", "", "PROCESS_EXITED"
            raise AssertionError(f"start-up must not run {command}")

        with tempfile.TemporaryDirectory() as scratch_name:
            adapter = self._adapter(Path(scratch_name), process, command_runner)
            adapter.start()
            try:
                self.assertEqual(calls, [
                    ["usbipd", "list"],
                    ["usbipd", "attach", "--wsl", "--busid", "2-1"],
                ])
                self.assertIn("attached from Shared", adapter.start_detail)
                self.assertIn("Connected to device", adapter.start_detail)
                # USBHostFS's start-up `waiting for device...` is not a device loss.
                self.assertFalse(adapter.take_waiting_for_device())
            finally:
                adapter.stop()

    def test_attached_device_start_waits_for_connected_without_attaching(self):
        process = FakeUsbHostFsProcess(USBHOSTFS_BANNER)
        calls = []

        def command_runner(command, timeout):
            calls.append(command)
            if command == ["usbipd", "list"]:
                process.stdout.emit("Connected to device")
                return self._usbipd_row("Attached")
            raise AssertionError(f"start-up must not run {command}")

        with tempfile.TemporaryDirectory() as scratch_name:
            adapter = self._adapter(Path(scratch_name), process, command_runner)
            adapter.start()
            adapter.stop()

        self.assertEqual(calls, [["usbipd", "list"]])
        self.assertIn("already attached", adapter.start_detail)

    def test_already_connected_server_needs_no_usbipd_command(self):
        process = FakeUsbHostFsProcess(("USBHostFS (c) TyRaNiD 2k6", "Connected to device"))

        def command_runner(command, timeout):
            raise AssertionError(f"start-up must not run {command}")

        with tempfile.TemporaryDirectory() as scratch_name:
            adapter = self._adapter(Path(scratch_name), process, command_runner)
            adapter.start()
            adapter.stop()

        self.assertEqual(adapter.start_detail, "USBHostFS reported `Connected to device`")

    def test_missing_connected_to_device_fails_start_within_its_budget(self):
        process = FakeUsbHostFsProcess(USBHOSTFS_BANNER)

        def command_runner(command, timeout):
            if command == ["usbipd", "list"]:
                return self._usbipd_row("Shared")
            return 0, "", "", "PROCESS_EXITED"

        with tempfile.TemporaryDirectory() as scratch_name:
            adapter = self._adapter(
                Path(scratch_name), process, command_runner, start_timeout=0.3
            )
            started = time.monotonic()
            with self.assertRaises(TransportStartError) as raised:
                adapter.start()
            elapsed = time.monotonic() - started
            adapter.stop()

        message = str(raised.exception)
        self.assertLess(elapsed, 5.0)
        self.assertIn("did not report `Connected to device` for PSPLink device 2-1", message)
        self.assertIn("within the 0.3s transport start budget", message)
        self.assertIn("lsusb -d 054c:01c9", message)
        self.assertIn("last usbhostfs_pc output: USBHostFS (c) TyRaNiD 2k6 | waiting for device...",
                      message)

    def test_server_exit_before_polling_fails_start_with_its_output(self):
        process = FakeUsbHostFsProcess(
            ("USBHostFS (c) TyRaNiD 2k6", "USB initialization failed: -1"), exits=True
        )

        def command_runner(command, timeout):
            raise AssertionError(f"start-up must not run {command}")

        with tempfile.TemporaryDirectory() as scratch_name:
            adapter = self._adapter(Path(scratch_name), process, command_runner)
            with self.assertRaises(TransportStartError) as raised:
                adapter.start()
            adapter.stop()

        message = str(raised.exception)
        self.assertIn("before usbhostfs_pc exited or closed its output", message)
        self.assertIn("USB initialization failed: -1", message)

    def test_unbound_device_fails_start_with_manual_bind_and_no_attach(self):
        process = FakeUsbHostFsProcess(USBHOSTFS_BANNER)
        calls = []

        def command_runner(command, timeout):
            calls.append(command)
            return self._usbipd_row("Not shared")

        with tempfile.TemporaryDirectory() as scratch_name:
            adapter = self._adapter(Path(scratch_name), process, command_runner)
            with self.assertRaises(TransportStartError) as raised:
                adapter.start()
            adapter.stop()

        self.assertEqual(calls, [["usbipd", "list"]])
        self.assertIn("usbipd bind --busid 2-1", str(raised.exception))

    def test_only_a_wait_after_a_connection_is_a_device_loss(self):
        with tempfile.TemporaryDirectory() as scratch_name:
            adapter = self._adapter(Path(scratch_name), None, None)
            adapter._observe_server_output("waiting for device...")
            self.assertFalse(adapter.take_waiting_for_device())
            adapter._observe_server_output("Connected to device")
            self.assertFalse(adapter.take_waiting_for_device())
            adapter._observe_server_output("Read cancelled (remote disconnected)")
            adapter._observe_server_output("waiting for device...")
            self.assertTrue(adapter.take_waiting_for_device())
            self.assertFalse(adapter.take_waiting_for_device())

    def test_start_timeout_must_be_finite_and_positive(self):
        with tempfile.TemporaryDirectory() as scratch_name:
            for value in (0.0, -1.0, float("inf"), float("nan")):
                with self.assertRaises(ValueError):
                    self._adapter(Path(scratch_name), None, None, start_timeout=value)


def _checkpoint_state(**fields):
    state = {
        "schema": 1,
        "campaign_id": "synthetic-campaign",
        "source_commit": SOURCE_COMMIT,
        "session_id": "synthetic-session",
        "queue": list(CAMPAIGN_QUEUE_CASES),
        "state": "IN_PROGRESS",
        "completed_cases": [],
        "interrupted_cases": [],
        "host0_qualified": False,
        "next_case_index": 0,
        "active_case_index": None,
        "active_case_id": None,
        "phase": None,
        "failed_case_id": None,
    }
    state.update(fields)
    return state


class CampaignCheckpointTransitionTests(unittest.TestCase):
    """Every durable checkpoint transition keeps a usable resume position."""

    CASES = len(CAMPAIGN_QUEUE_CASES)

    def test_case_start_records_reset_and_launch_phases(self):
        base = _checkpoint_state(next_case_index=3, host0_qualified=True)
        reset = _checkpoint_case_started(base, 3, "wait-outcomes", "RESET_BEFORE_CASE")
        launch = _checkpoint_case_started(reset, 3, "wait-outcomes", "CASE_ACTIVE")
        self.assertEqual(
            (reset["state"], reset["phase"], reset["active_case_index"], reset["next_case_index"]),
            ("RUNNING", "RESET_BEFORE_CASE", 3, 3),
        )
        self.assertEqual((launch["phase"], launch["active_case_id"]), ("CASE_ACTIVE", "wait-outcomes"))
        with self.assertRaises(ValueError):
            _checkpoint_case_started(base, 3, "wait-outcomes", "UNKNOWN")

    def test_clean_case_finish_advances_and_records_completion(self):
        running = _checkpoint_case_started(
            _checkpoint_state(), 0, "transport-write", "CASE_ACTIVE"
        )
        finished = _checkpoint_case_finished(
            running, 0, "transport-write",
            intervention_case_id=None, resume_case_index=None, host0_qualified=True,
        )
        self.assertEqual(finished["state"], "IN_PROGRESS")
        self.assertEqual(finished["next_case_index"], 1)
        self.assertEqual(finished["completed_cases"], ["transport-write"])
        self.assertTrue(finished["host0_qualified"])
        self.assertIsNone(finished["active_case_index"])
        self.assertIsNone(finished["phase"])

    def test_interrupted_case_finish_waits_for_power_cycle_at_the_next_case(self):
        running = _checkpoint_case_started(
            _checkpoint_state(next_case_index=1, host0_qualified=True,
                              completed_cases=["transport-write"]),
            1, "kernel-alarm", "CASE_ACTIVE",
        )
        finished = _checkpoint_case_finished(
            running, 1, "kernel-alarm",
            intervention_case_id="kernel-alarm", resume_case_index=2, host0_qualified=True,
        )
        self.assertEqual(finished["state"], "WAITING_FOR_POWER_CYCLE")
        self.assertEqual(finished["next_case_index"], 2)
        self.assertEqual(finished["failed_case_id"], "kernel-alarm")
        self.assertEqual(finished["interrupted_cases"], ["kernel-alarm"])
        self.assertEqual(finished["completed_cases"], ["transport-write"])

        defaulted = _checkpoint_case_finished(
            running, 1, "kernel-alarm",
            intervention_case_id="kernel-alarm", resume_case_index=None, host0_qualified=True,
        )
        self.assertEqual(defaulted["next_case_index"], 2)

    def test_interrupted_preflight_without_host0_qualification_resumes_at_the_preflight(self):
        running = _checkpoint_case_started(_checkpoint_state(), 0, "transport-write", "CASE_ACTIVE")
        finished = _checkpoint_case_finished(
            running, 0, "transport-write",
            intervention_case_id="transport-write", resume_case_index=1, host0_qualified=False,
        )
        self.assertEqual(finished["state"], "WAITING_FOR_POWER_CYCLE")
        self.assertEqual(finished["next_case_index"], 0)
        self.assertFalse(finished["host0_qualified"])

    def test_run_end_keeps_a_per_case_power_cycle_record(self):
        waiting = _checkpoint_state(
            state="WAITING_FOR_POWER_CYCLE", next_case_index=2, failed_case_id="kernel-alarm",
            host0_qualified=True,
        )
        self.assertEqual(
            _checkpoint_after_run(
                waiting, terminal_reason="PHYSICAL_INTERVENTION_REQUIRED",
                intervention_case_id="kernel-alarm", resume_case_index=2, case_count=self.CASES,
            ),
            waiting,
        )

    def test_run_end_marks_a_finished_queue_complete(self):
        done = _checkpoint_state(next_case_index=self.CASES, host0_qualified=True)
        final = _checkpoint_after_run(
            done, terminal_reason=None, intervention_case_id=None,
            resume_case_index=None, case_count=self.CASES,
        )
        self.assertEqual(final["state"], "COMPLETE")
        partial = _checkpoint_state(next_case_index=4, host0_qualified=True)
        self.assertEqual(
            _checkpoint_after_run(
                partial, terminal_reason=None, intervention_case_id=None,
                resume_case_index=None, case_count=self.CASES,
            ),
            partial,
        )

    def test_transport_start_failure_keeps_the_resume_position_without_power_cycle(self):
        # The 2026-10-08 failure: resumed after a confirmed power cycle at case 1, then the
        # transport did not start. The position must survive and no confirmation is owed.
        resumed = _checkpoint_state(
            next_case_index=1, host0_qualified=True, interrupted_cases=["transport-write"]
        )
        final = _checkpoint_after_run(
            resumed, terminal_reason="TRANSPORT_START_FAILED", intervention_case_id=None,
            resume_case_index=None, case_count=self.CASES,
        )
        self.assertEqual(final["state"], "IN_PROGRESS")
        self.assertEqual(final["next_case_index"], 1)
        self.assertEqual(final["interrupted_cases"], ["transport-write"])
        self.assertIsNone(final["failed_case_id"])

    def test_intervention_without_a_runner_position_never_clobbers_the_checkpoint(self):
        resumed = _checkpoint_state(next_case_index=1, host0_qualified=True)
        final = _checkpoint_after_run(
            resumed, terminal_reason="PHYSICAL_INTERVENTION_REQUIRED",
            intervention_case_id=None, resume_case_index=None, case_count=self.CASES,
        )
        self.assertEqual(final["state"], "WAITING_FOR_POWER_CYCLE")
        self.assertEqual(final["next_case_index"], 1)

    def test_stop_after_reset_but_before_launch_resumes_that_case_without_power_cycle(self):
        reset = _checkpoint_case_started(
            _checkpoint_state(next_case_index=5, host0_qualified=True),
            5, "refer-status-size", "RESET_BEFORE_CASE",
        )
        final = _checkpoint_after_run(
            reset, terminal_reason="TEARDOWN_S0_SNAPSHOT_FAILED", intervention_case_id=None,
            resume_case_index=None, case_count=self.CASES,
        )
        self.assertEqual(final["state"], "IN_PROGRESS")
        self.assertEqual(final["next_case_index"], 5)
        self.assertIsNone(final["active_case_index"])

    def test_failed_reset_waits_for_power_cycle_at_the_unlaunched_case(self):
        reset = _checkpoint_case_started(
            _checkpoint_state(next_case_index=5, host0_qualified=True),
            5, "refer-status-size", "RESET_BEFORE_CASE",
        )
        final = _checkpoint_after_run(
            reset, terminal_reason="PHYSICAL_INTERVENTION_REQUIRED",
            intervention_case_id="ge-break-continue", resume_case_index=5, case_count=self.CASES,
        )
        self.assertEqual(final["state"], "WAITING_FOR_POWER_CYCLE")
        self.assertEqual(final["next_case_index"], 5)
        self.assertEqual(final["failed_case_id"], "ge-break-continue")

    def test_launched_case_without_completion_record_waits_for_power_cycle(self):
        kernel_index = CAMPAIGN_QUEUE_CASES.index("kernel-misc")
        launched = _checkpoint_case_started(
            _checkpoint_state(next_case_index=kernel_index, host0_qualified=True),
            kernel_index, "kernel-misc", "CASE_ACTIVE",
        )
        final = _checkpoint_after_run(
            launched, terminal_reason="CHECKPOINT_WRITE_FAILED", intervention_case_id=None,
            resume_case_index=None, case_count=self.CASES,
        )
        self.assertEqual(final["state"], "WAITING_FOR_POWER_CYCLE")
        self.assertEqual(final["next_case_index"], kernel_index + 1)
        self.assertEqual(final["failed_case_id"], "kernel-misc")

    def test_running_checkpoint_without_active_index_is_rejected(self):
        with self.assertRaises(ValueError):
            _checkpoint_after_run(
                _checkpoint_state(state="RUNNING"), terminal_reason="TRANSPORT_START_FAILED",
                intervention_case_id=None, resume_case_index=None, case_count=self.CASES,
            )

    def test_resume_without_checkpoint(self):
        self.assertEqual(
            _checkpoint_resume(None, confirm_power_cycle=False, case_count=self.CASES),
            (0, {"completed_cases": [], "interrupted_cases": [], "host0_qualified": False}),
        )
        refused = _checkpoint_resume(None, confirm_power_cycle=True, case_count=self.CASES)
        self.assertEqual(refused["status"], "REFUSED")

    def test_resume_complete_checkpoint(self):
        result = _checkpoint_resume(
            _checkpoint_state(state="COMPLETE", next_case_index=self.CASES),
            confirm_power_cycle=False, case_count=self.CASES,
        )
        self.assertEqual(result["status"], "COMPLETE")

    def test_resume_running_checkpoint_requires_confirmation_and_skips_a_launched_case(self):
        launched = _checkpoint_state(
            state="RUNNING", active_case_index=4, active_case_id="ge-break-continue",
            phase="CASE_ACTIVE", next_case_index=4, host0_qualified=True,
        )
        waiting = _checkpoint_resume(launched, confirm_power_cycle=False, case_count=self.CASES)
        self.assertEqual(waiting["status"], "WAITING_FOR_POWER_CYCLE_CONFIRMATION")
        start, carried = _checkpoint_resume(launched, confirm_power_cycle=True, case_count=self.CASES)
        self.assertEqual(start, 5)
        self.assertTrue(carried["host0_qualified"])
        reset = dict(launched, phase="RESET_BEFORE_CASE")
        self.assertEqual(
            _checkpoint_resume(reset, confirm_power_cycle=True, case_count=self.CASES)[0], 4
        )
        for broken in (dict(launched, active_case_index=None), dict(launched, phase=None)):
            refused = _checkpoint_resume(broken, confirm_power_cycle=True, case_count=self.CASES)
            self.assertEqual(refused["status"], "REFUSED")

    def test_resume_waiting_checkpoint_requires_confirmation(self):
        waiting = _checkpoint_state(
            state="WAITING_FOR_POWER_CYCLE", next_case_index=1, failed_case_id="transport-write",
            host0_qualified=True,
        )
        status = _checkpoint_resume(waiting, confirm_power_cycle=False, case_count=self.CASES)
        self.assertEqual(status["status"], "WAITING_FOR_POWER_CYCLE_CONFIRMATION")
        self.assertEqual(status["resume_case_index"], 1)
        self.assertEqual(
            _checkpoint_resume(waiting, confirm_power_cycle=True, case_count=self.CASES)[0], 1
        )
        clobbered = dict(waiting, next_case_index=None)
        refused = _checkpoint_resume(clobbered, confirm_power_cycle=True, case_count=self.CASES)
        self.assertEqual(refused, {"status": "REFUSED", "reason": "invalid resume case index"})

    def test_resume_in_progress_checkpoint_refuses_a_confirmation(self):
        in_progress = _checkpoint_state(next_case_index=1, host0_qualified=True)
        self.assertEqual(
            _checkpoint_resume(in_progress, confirm_power_cycle=False, case_count=self.CASES)[0], 1
        )
        refused = _checkpoint_resume(in_progress, confirm_power_cycle=True, case_count=self.CASES)
        self.assertEqual(refused["status"], "REFUSED")
        self.assertIn("case index 1", refused["reason"])

    def test_resume_rejects_unknown_state_and_non_integer_positions(self):
        for checkpoint in (
            _checkpoint_state(state="PAUSED"),
            _checkpoint_state(next_case_index=True, host0_qualified=True),
            _checkpoint_state(next_case_index=self.CASES + 1, host0_qualified=True),
            _checkpoint_state(next_case_index=-1),
        ):
            result = _checkpoint_resume(checkpoint, confirm_power_cycle=False, case_count=self.CASES)
            self.assertEqual(result["status"], "REFUSED")

    def test_resume_past_the_preflight_requires_recorded_host0_qualification(self):
        legacy = _checkpoint_state(next_case_index=1)
        del legacy["host0_qualified"]
        for checkpoint in (legacy, _checkpoint_state(next_case_index=1)):
            refused = _checkpoint_resume(checkpoint, confirm_power_cycle=False, case_count=self.CASES)
            self.assertEqual(refused["status"], "REFUSED")
            self.assertIn("transport-write host0 preflight", refused["reason"])
        start, _carried = _checkpoint_resume(
            _checkpoint_state(next_case_index=0), confirm_power_cycle=False, case_count=self.CASES
        )
        self.assertEqual(start, 0)


class CampaignPlanCheckpointTests(unittest.TestCase):
    """run_campaign_plan writes the transitions above around a simulated PSPLink."""

    def _plan(self, scratch: Path) -> Path:
        host0_root = scratch / "host0"
        host0_root.mkdir()
        cases = []
        for case_id in CAMPAIGN_QUEUE_CASES:
            (host0_root / f"{case_id}.prx").write_bytes(b"synthetic PRX")
            cases.append({"case_id": case_id, "prx": f"{case_id}.prx", "timeout_seconds": 0.2})
        plan_path = scratch / "campaign-plan.json"
        plan_path.write_text(json.dumps({
            "schema": 1,
            "campaign_id": "synthetic-campaign",
            "session_id": "synthetic-session",
            "source_commit": SOURCE_COMMIT,
            "console_model": "PSP-3000",
            "host0_root": "host0",
            "report_path": "report.json",
            "checkpoint_path": "checkpoint.json",
            "cases": cases,
        }), encoding="utf-8")
        return plan_path

    def _run(self, plan_path: Path, *, confirm: bool, transport: SimulatedPsplinkTransport):
        def factory(**kwargs):
            transport.host0_root = kwargs["host0_root"]
            return transport

        with patch.object(run_psplink_module, "_read_hardware_lock",
                          return_value=(True, "HELD_AND_CONFIRMED")), \
                patch.object(run_psplink_module, "_check_source_tree", return_value=None):
            return run_campaign_plan(
                plan_path,
                dry_run=False,
                confirm_power_cycle=confirm,
                pspsh_argv=["pspsh", "-e", "{remote_command}"],
                usbhostfs_argv=["usbhostfs_pc", "{host0_root}"],
                transport_factory=factory,
            )

    @staticmethod
    def _checkpoint(plan_path: Path) -> dict:
        return json.loads((plan_path.parent / "checkpoint.json").read_text(encoding="utf-8"))

    @staticmethod
    def _launched(transport: SimulatedPsplinkTransport) -> list[str]:
        return [
            Path(command).name.removesuffix(".prx")
            for command, _timeout in transport.commands
            if command.startswith("ldstart ")
        ]

    def test_transport_start_failure_after_confirmed_power_cycle_keeps_the_resume_position(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="campaign-checkpoint-", dir=fixture_dir) as scratch_name:
            plan_path = self._plan(Path(scratch_name))

            # Run 1: the preflight passes; kernel-alarm's probe never finishes.
            first = SimulatedPsplinkTransport(
                transport_file_cases={"transport-write"}, unfinished_cases={"kernel-alarm"}
            )
            code, report = self._run(plan_path, confirm=False, transport=first)
            self.assertEqual(code, 3)
            self.assertEqual(self._launched(first), ["transport-write", "kernel-alarm"])
            checkpoint = self._checkpoint(plan_path)
            self.assertEqual(checkpoint["state"], "WAITING_FOR_POWER_CYCLE")
            self.assertEqual(checkpoint["next_case_index"], 2)
            self.assertEqual(checkpoint["completed_cases"], ["transport-write"])
            self.assertEqual(checkpoint["interrupted_cases"], ["kernel-alarm"])
            self.assertTrue(checkpoint["host0_qualified"])

            # Run 2: power cycle confirmed, but the shell never qualifies before any launch.
            second = SimulatedPsplinkTransport(fail_all_ver=True)
            code, report = self._run(plan_path, confirm=True, transport=second)
            self.assertEqual(code, 2)
            self.assertEqual(report["terminal_reason"], "TRANSPORT_START_FAILED")
            self.assertEqual(self._launched(second), [])
            checkpoint = self._checkpoint(plan_path)
            self.assertEqual(checkpoint["state"], "IN_PROGRESS")
            self.assertEqual(checkpoint["next_case_index"], 2)
            self.assertEqual(report["checkpoint_next_case_index"], 2)
            self.assertEqual(checkpoint["completed_cases"], ["transport-write"])
            self.assertTrue(checkpoint["host0_qualified"])

            # Run 3: a repeated confirmation is refused and changes nothing.
            code, report = self._run(
                plan_path, confirm=True, transport=SimulatedPsplinkTransport()
            )
            self.assertEqual(code, 2)
            self.assertEqual(report["status"], "REFUSED")
            self.assertEqual(self._checkpoint(plan_path), checkpoint)

            # Run 4: without a confirmation the queue resumes at the preserved case.
            fourth = SimulatedPsplinkTransport(unfinished_cases={"thread-scheduler"})
            code, report = self._run(plan_path, confirm=False, transport=fourth)
            self.assertEqual(code, 3)
            self.assertEqual(self._launched(fourth), ["thread-scheduler"])
            self.assertNotIn("reset", [command for command, _timeout in fourth.commands])
            checkpoint = self._checkpoint(plan_path)
            self.assertEqual(checkpoint["state"], "WAITING_FOR_POWER_CYCLE")
            self.assertEqual(checkpoint["next_case_index"], 3)
            self.assertEqual(checkpoint["interrupted_cases"], ["kernel-alarm", "thread-scheduler"])

    def test_fresh_campaign_transport_start_failure_stays_at_the_preflight(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="campaign-start-fail-", dir=fixture_dir) as scratch_name:
            plan_path = self._plan(Path(scratch_name))
            transport = SimulatedPsplinkTransport(start_error="usbhostfs_pc printed nothing")
            code, report = self._run(plan_path, confirm=False, transport=transport)
            checkpoint = self._checkpoint(plan_path)

        self.assertEqual(code, 2)
        self.assertEqual(report["terminal_reason"], "TRANSPORT_START_FAILED")
        self.assertEqual(checkpoint["state"], "IN_PROGRESS")
        self.assertEqual(checkpoint["next_case_index"], 0)
        self.assertEqual(transport.commands, [])

    def test_failed_preflight_round_trip_resumes_at_the_preflight(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="campaign-preflight-", dir=fixture_dir) as scratch_name:
            plan_path = self._plan(Path(scratch_name))
            transport = SimulatedPsplinkTransport(fail_host0_roundtrip_cases={"transport-write"})
            code, report = self._run(plan_path, confirm=False, transport=transport)
            checkpoint = self._checkpoint(plan_path)

        self.assertEqual(code, 3)
        self.assertEqual(report["intervention_case_id"], "transport-write")
        self.assertEqual(checkpoint["state"], "WAITING_FOR_POWER_CYCLE")
        self.assertEqual(checkpoint["next_case_index"], 0)
        self.assertFalse(checkpoint["host0_qualified"])



class HardwareLockGateTests(unittest.TestCase):
    """Every PSP-touching mode passes the one hardware-lock gate, re-read each time."""

    def test_gate_requires_a_valid_session_and_a_held_lock(self):
        for session in (None, "", "bad session", "x" * 65, 7):
            with self.assertRaises(HardwareLockError) as raised:
                run_psplink_module.require_hardware_lock(session)
            self.assertEqual(raised.exception.status, "HARDWARE_LOCK_SESSION_REQUIRED")
        with patch.object(run_psplink_module, "_read_hardware_lock",
                          return_value=(False, "HARDWARE_LOCK_SESSION_MISMATCH")):
            with self.assertRaises(HardwareLockError) as raised:
                run_psplink_module.require_hardware_lock("synthetic-session")
        self.assertEqual(raised.exception.status, "HARDWARE_LOCK_SESSION_MISMATCH")
        with patch.object(run_psplink_module, "_read_hardware_lock", _lock_held):
            run_psplink_module.require_hardware_lock("synthetic-session")

    def test_lock_file_contract(self):
        with tempfile.TemporaryDirectory() as scratch_name:
            lock_path = Path(scratch_name) / "HARDWARE_LOCK.json"
            cases = (
                (None, "HARDWARE_LOCK_UNAVAILABLE:FileNotFoundError"),
                ({"state": "FREE"}, "HARDWARE_LOCK_NOT_HELD"),
                ({"state": "HELD", "holder_session": "s"}, "HARDWARE_LOCK_POWER_CYCLE_NOT_CONFIRMED"),
                ({"state": "HELD", "power_cycle_confirmed": True, "holder_session": "other"},
                 "HARDWARE_LOCK_SESSION_MISMATCH"),
                ({"state": "HELD", "power_cycle_confirmed": True, "holder_session": "s"},
                 "HELD_AND_CONFIRMED"),
            )
            with patch.object(run_psplink_module, "HARDWARE_LOCK_PATH", lock_path):
                for content, expected in cases:
                    if content is None:
                        lock_path.unlink(missing_ok=True)
                    else:
                        lock_path.write_text(json.dumps(content), encoding="utf-8")
                    held, status = run_psplink_module._read_hardware_lock("s")
                    self.assertEqual(status, expected)
                    self.assertEqual(held, expected == "HELD_AND_CONFIRMED")

    def test_process_transport_checks_the_lock_before_spawning_usbhostfs(self):
        spawned = []
        with tempfile.TemporaryDirectory() as scratch_name, \
                patch.object(run_psplink_module, "_read_hardware_lock",
                             return_value=(False, "HARDWARE_LOCK_NOT_HELD")):
            adapter = PsplinkProcessTransport(
                session_id="synthetic-session",
                pspsh_argv=["fake-pspsh"],
                usbhostfs_argv=["fake-usbhostfs"],
                host0_root=Path(scratch_name),
                command_runner=lambda command, timeout: self.fail(f"ran {command}"),
                popen_factory=lambda *args, **kwargs: spawned.append(args),
            )
            with self.assertRaises(HardwareLockError) as raised:
                adapter.start()
        self.assertEqual(raised.exception.status, "HARDWARE_LOCK_NOT_HELD")
        self.assertEqual(spawned, [])

    def _transport_write(self, scratch: Path) -> CampaignCase:
        binary = scratch / "transport-write.prx"
        binary.write_bytes(b"synthetic PRX")
        return CampaignCase("transport-write", binary, 1.0)

    def test_runner_refuses_at_start_without_any_psplink_command(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="lock-start-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            transport = SimulatedPsplinkTransport(lock_held_checks=0)
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport, console_model="PSP-3000-04g", source_commit=SOURCE_COMMIT,
            ).run([self._transport_write(scratch)])

        self.assertEqual(report["terminal_reason"], "HARDWARE_LOCK_REFUSED")
        self.assertEqual(report["hardware_lock_status"], "HARDWARE_LOCK_NOT_HELD")
        self.assertEqual(transport.commands, [])
        self.assertFalse(transport.started)
        self.assertTrue(transport.stopped)

    def test_runner_rechecks_the_lock_before_each_reset_and_launch(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        for held_checks, expected_launches, expected_resets in (
            # start, case-1 entry, case-1 launch pass; case-2 entry refuses before its reset
            (3, ["transport-write"], 0),
            # ... case-2 entry passes; L2 reset gate refuses before `reset`
            (4, ["transport-write"], 0),
            # ... reset passes; the pre-launch check refuses before `ldstart`
            (5, ["transport-write"], 1),
        ):
            with self.subTest(held_checks=held_checks), tempfile.TemporaryDirectory(
                prefix="lock-recheck-", dir=fixture_dir
            ) as scratch_name:
                scratch = Path(scratch_name)
                cases = [self._transport_write(scratch)]
                binary = scratch / "model-profile.prx"
                binary.write_bytes(b"synthetic PRX")
                cases.append(CampaignCase("model-profile", binary, 1.0))
                transport = SimulatedPsplinkTransport(
                    transport_file_cases={"transport-write"}, lock_held_checks=held_checks,
                )
                transport.host0_root = scratch
                report = PsplinkCampaignRunner(
                    transport, console_model="PSP-3000-04g", source_commit=SOURCE_COMMIT,
                    model_code=3,
                ).run(cases, reset_between_cases=True, stop_on_incomplete=True)
                commands = [command for command, _timeout in transport.commands]
                launches = [
                    Path(command).name.removesuffix(".prx")
                    for command in commands if command.startswith("ldstart ")
                ]
                self.assertEqual(report["terminal_reason"], "HARDWARE_LOCK_REFUSED")
                self.assertIsNone(report["intervention_case_id"])
                self.assertEqual(launches, expected_launches)
                self.assertEqual(commands.count("reset"), expected_resets)
                self.assertEqual(len(report["envelopes"]), 1)

    def test_campaign_plan_refuses_without_writing_a_checkpoint(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="lock-plan-", dir=fixture_dir) as scratch_name:
            plan_path = CampaignPlanCheckpointTests()._plan(Path(scratch_name))
            factory_calls = []
            with patch.object(run_psplink_module, "_read_hardware_lock",
                              return_value=(False, "HARDWARE_LOCK_POWER_CYCLE_NOT_CONFIRMED")):
                code, report = run_campaign_plan(
                    plan_path, dry_run=False, confirm_power_cycle=False,
                    pspsh_argv=["pspsh"], usbhostfs_argv=["usbhostfs_pc"],
                    transport_factory=lambda **kwargs: factory_calls.append(kwargs),
                )
            checkpoint_written = (plan_path.parent / "checkpoint.json").exists()

        self.assertEqual(code, 2)
        self.assertEqual(report["reason"], "HARDWARE_LOCK_POWER_CYCLE_NOT_CONFIRMED")
        self.assertEqual(factory_calls, [])
        self.assertFalse(checkpoint_written)

    def test_campaign_plan_revoked_lock_stops_before_the_next_case_without_power_cycle(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="lock-plan-revoke-", dir=fixture_dir) as scratch_name:
            helper = CampaignPlanCheckpointTests()
            plan_path = helper._plan(Path(scratch_name))
            transport = SimulatedPsplinkTransport(
                transport_file_cases={"transport-write"}, lock_held_checks=3,
            )
            code, report = helper._run(plan_path, confirm=False, transport=transport)
            checkpoint = helper._checkpoint(plan_path)

        self.assertEqual(code, 2)
        self.assertEqual(report["terminal_reason"], "HARDWARE_LOCK_REFUSED")
        self.assertEqual(checkpoint["state"], "IN_PROGRESS")
        self.assertEqual(checkpoint["next_case_index"], 1)
        self.assertEqual(checkpoint["completed_cases"], ["transport-write"])

    def test_campaign_plan_rejects_a_separate_session_flag(self):
        with self.assertRaises(SystemExit):
            run_psplink_module.main([
                "--campaign-plan", "plan.json", "--session-id", "synthetic-session",
            ])

    def _campaign_case_argv(self, scratch: Path, *extra: str) -> list[str]:
        binary = scratch / "transport-write.prx"
        binary.write_bytes(b"synthetic PRX")
        return [
            "--campaign-case", f"transport-write={binary}",
            "--host0-root", str(scratch),
            "--model", "PSP-3000",
            "--source-commit", SOURCE_COMMIT,
            "--usbhostfs-argv-json", json.dumps(["nakagawa-missing-usbhostfs-binary"]),
            "--out", str(scratch / "report.json"),
            *extra,
        ]

    def test_campaign_case_mode_requires_a_session_id(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="lock-case-", dir=fixture_dir) as scratch_name:
            with self.assertRaises(SystemExit) as raised:
                run_psplink_module.main(self._campaign_case_argv(Path(scratch_name)))
        self.assertEqual(raised.exception.code, 2)

    def test_campaign_case_mode_refuses_before_spawning_the_transport(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="lock-case-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            argv = self._campaign_case_argv(scratch, "--session-id", "synthetic-session")
            with patch.object(run_psplink_module, "_read_hardware_lock",
                              return_value=(False, "HARDWARE_LOCK_SESSION_MISMATCH")):
                code = run_psplink_module.main(argv)
            report = json.loads((scratch / "report.json").read_text(encoding="utf-8"))

        # A missing usbhostfs binary would have ended TRANSPORT_START_FAILED; the lock
        # refusal proves the gate ran first.
        self.assertEqual(code, 2)
        self.assertEqual(report["terminal_reason"], "HARDWARE_LOCK_REFUSED")
        self.assertEqual(report["hardware_lock_status"], "HARDWARE_LOCK_SESSION_MISMATCH")
        self.assertEqual(report["envelopes"], [])

    def test_command_mode_requires_a_session_and_a_held_lock(self):
        with tempfile.TemporaryDirectory() as scratch_name:
            out = Path(scratch_name) / "capture-report.json"
            command = ["--command", "pspsh -e ver", "--out", str(out)]
            with patch.object(run_psplink_module, "_run_command") as run_command:
                with self.assertRaises(SystemExit):
                    run_psplink_module.main(command)
                with patch.object(run_psplink_module, "_read_hardware_lock",
                                  return_value=(False, "HARDWARE_LOCK_NOT_HELD")):
                    code = run_psplink_module.main(command + ["--session-id", "synthetic-session"])
                run_command.assert_not_called()
            report = json.loads(out.read_text(encoding="utf-8"))

        self.assertEqual(code, 2)
        self.assertEqual(report["status"], "REFUSED")
        self.assertEqual(report["reason"], "HARDWARE_LOCK_NOT_HELD")



class ExpectedFirmwareFormTests(unittest.TestCase):
    """An expected firmware must use the exact `pspver` form it is compared with."""

    def _plan_with(self, scratch: Path, **fields) -> Path:
        plan_path = CampaignPlanCheckpointTests()._plan(scratch)
        plan = json.loads(plan_path.read_text(encoding="utf-8"))
        plan.update(fields)
        plan_path.write_text(json.dumps(plan), encoding="utf-8")
        return plan_path

    def _dry_run(self, plan_path: Path):
        with patch.object(run_psplink_module, "_check_source_tree", return_value=None):
            return run_campaign_plan(
                plan_path, dry_run=True, confirm_power_cycle=False,
                pspsh_argv=["pspsh"], usbhostfs_argv=["usbhostfs_pc"],
            )

    def test_plan_rejects_firmware_that_cannot_match_pspver(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        for value in ("6.61", "6.61-ARK", "6.6.1 ", "v6.6.1", "", 661, ["6.6.1"]):
            with self.subTest(value=value), tempfile.TemporaryDirectory(
                prefix="fw-plan-", dir=fixture_dir
            ) as scratch_name:
                code, report = self._dry_run(
                    self._plan_with(Path(scratch_name), expected_firmware=value)
                )
                self.assertEqual(code, 2)
                self.assertEqual(report["status"], "REFUSED")
                self.assertIn("expected_firmware", report["reason"])
                self.assertIn("for example 6.6.1 for firmware 6.61", report["reason"])

    def test_plan_accepts_the_pspver_form_and_an_absent_field(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        for fields in ({"expected_firmware": "6.6.1"}, {}):
            with self.subTest(fields=fields), tempfile.TemporaryDirectory(
                prefix="fw-plan-ok-", dir=fixture_dir
            ) as scratch_name:
                code, report = self._dry_run(self._plan_with(Path(scratch_name), **fields))
                self.assertEqual((code, report["status"]), (0, "VALIDATED_OFFLINE"))

    def test_plan_rejects_a_model_code_that_would_be_silently_ignored(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        for value in ("3", True, -1, 3.0):
            with self.subTest(value=value), tempfile.TemporaryDirectory(
                prefix="model-plan-", dir=fixture_dir
            ) as scratch_name:
                code, report = self._dry_run(self._plan_with(Path(scratch_name), model_code=value))
                self.assertEqual(code, 2)
                self.assertIn("model_code", report["reason"])

    def test_runner_and_campaign_case_cli_reject_the_firmware_marketing_name(self):
        with self.assertRaises(ValueError) as raised:
            PsplinkCampaignRunner(
                SimulatedPsplinkTransport(), console_model="PSP-3000",
                source_commit=SOURCE_COMMIT, expected_firmware="6.61",
            )
        self.assertIn("6.6.1", str(raised.exception))
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="fw-cli-", dir=fixture_dir) as scratch_name:
            argv = HardwareLockGateTests()._campaign_case_argv(
                Path(scratch_name), "--session-id", "synthetic-session", "--firmware", "6.61"
            )
            with self.assertRaises(SystemExit) as exited:
                run_psplink_module.main(argv)
        self.assertEqual(exited.exception.code, 2)

    def test_pspver_form_matches_what_the_runner_parses(self):
        transport = SimulatedPsplinkTransport()
        runner = PsplinkCampaignRunner(
            transport, console_model="PSP-3000", source_commit=SOURCE_COMMIT,
            expected_firmware="6.6.1",
        )
        self.assertTrue(runner._qualify())
        self.assertEqual(runner.firmware, "6.6.1")



CONSOLE_CAPTURES = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle_captures"
# Verbatim host0 logs from the 2026-10-08 PSP-3000 session, written by the
# project-authored synthetic probes. They hold kernel object UIDs, return codes
# and counts, a placeholder binary digest and a project commit id: no serial,
# MAC, or retail data.
COMPLETE_CONSOLE_CAPTURES = {
    "transport_write.20261008-run3.txt": "transport-write",
    "kernel_alarm.20261008-run3.txt": "kernel-alarm",
    "kernel_alarm.20261008-manual.txt": "kernel-alarm",
    "thread_scheduler.20261008-manual.txt": "thread-scheduler",
    "refer_status_size.20261008-manual.txt": "refer-status-size",
    "display_wait_late.20261008-run5.txt": "display-wait-late",
}
PARTIAL_CONSOLE_CAPTURES = {
    "ge_break_continue.20261008-manual-partial.txt": "ge-break-continue",
    "registry_readonly.20261008-manual-partial.txt": "registry-readonly",
}


def _console_capture(name: str) -> str:
    return (CONSOLE_CAPTURES / name).read_text(encoding="utf-8")


class ConsoleCaptureRegressionTests(unittest.TestCase):
    """Real console captures must flow through every runner stage without crashing."""

    def test_complete_captures_parse_to_typed_records(self):
        for name, case_id in COMPLETE_CONSOLE_CAPTURES.items():
            with self.subTest(capture=name):
                text = _console_capture(name)
                self.assertTrue(run_psplink_module._campaign_stream_complete(text, case_id))
                records = text.split("NAKAGAWA_PSP_COMPLETE", 1)[0]
                parsed = _parse_campaign_records(
                    run_psplink_module._normalise_unbound_identity_fields(records), case_id
                )
                self.assertIsInstance(parsed, ParsedOutput)
                self.assertTrue(parsed.results)
                for record in parsed.results:
                    self.assertIsInstance(record, TestResult)
                    self.assertEqual(record.status, "PASS")

    def test_probe_success_check_accepts_complete_captures(self):
        # The 2026-10-08 crash: SequenceReport.results is keyed by case_id, so the
        # success check iterated strings and raised AttributeError on `.status`.
        for name, case_id in COMPLETE_CONSOLE_CAPTURES.items():
            with self.subTest(capture=name):
                now = time.time_ns()
                self.assertTrue(PsplinkCampaignRunner._probe_case_succeeded(
                    CampaignCase(case_id, Path(f"{case_id}.prx"), 1.0),
                    (0, "", "", "PROCESS_EXITED"),
                    "0x04280001",
                    run_started_ns=now,
                    host0_log_cleared=True,
                    captured_host0_text=_console_capture(name),
                    captured_host0_mtime_ns=now,
                    host0_capture_problem=None,
                ))

    def test_partial_captures_are_incomplete_named_errors(self):
        for name, case_id in PARTIAL_CONSOLE_CAPTURES.items():
            with self.subTest(capture=name):
                text = _console_capture(name)
                self.assertFalse(run_psplink_module._campaign_stream_complete(text, case_id))
                with self.assertRaises(PspProtocolError):
                    _parse_campaign_records(
                        run_psplink_module._normalise_unbound_identity_fields(text), case_id
                    )

    def test_every_queued_case_returns_the_typed_record_view(self):
        stream = CAMPAIGN_META + (
            "NAKAGAWA_PSP_TEST schema=1 test_id=SYNTHETIC case_id=row "
            "status=PASS result=0x0\n"
        )
        for case_id in CAMPAIGN_QUEUE_CASES:
            with self.subTest(case_id=case_id):
                self.assertNotEqual(
                    run_psplink_module._campaign_completeness_contract(case_id),
                    "unregistered-no-completion-contract",
                )
                with patch.object(run_psplink_module, "_validate_campaign_contract") as check:
                    parsed = _parse_campaign_records(stream, case_id)
                check.assert_called_once_with(stream, case_id)
                self.assertIsInstance(parsed, ParsedOutput)
                self.assertTrue(all(isinstance(item, TestResult) for item in parsed.results))

    def _campaign(self, scratch: Path, case_ids, **transport_options):
        cases = []
        for case_id in case_ids:
            binary = scratch / f"{case_id}.prx"
            binary.write_bytes(b"synthetic PRX")
            cases.append(CampaignCase(case_id, binary, 0.3))
        transport = SimulatedPsplinkTransport(
            transport_file_cases={"transport-write"}, **transport_options
        )
        transport.host0_root = scratch
        runner = PsplinkCampaignRunner(
            transport, console_model="PSP-3000-04g", source_commit=SOURCE_COMMIT, model_code=3,
        )
        return runner, transport, cases

    def test_campaign_runs_real_kernel_alarm_capture_end_to_end(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="capture-alarm-", dir=fixture_dir) as scratch_name:
            runner, transport, cases = self._campaign(
                Path(scratch_name), ("transport-write", "kernel-alarm"),
                host0_log_contents={
                    "kernel-alarm": _console_capture("kernel_alarm.20261008-run3.txt")
                },
            )
            report = runner.run(cases, reset_between_cases=True, stop_on_incomplete=True)

        self.assertIsNone(report["terminal_reason"])
        alarm = report["envelopes"][1]
        self.assertEqual(alarm["CASE_ID"], "kernel-alarm")
        self.assertEqual(alarm["TEARDOWN_CHECK"]["status"], "PASS")
        self.assertIn("case_id=kernel-alarm-done status=PASS", alarm["RAW_RESULT"])
        # The capture was built from another commit: bound and reported, never hidden.
        self.assertEqual(alarm["SOURCE_COMMIT_BINDING"], "MISMATCH")
        self.assertFalse(alarm["ACCEPTANCE_ELIGIBLE"])

    def test_campaign_partial_console_capture_stops_as_an_incomplete_case(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        for name, case_id in PARTIAL_CONSOLE_CAPTURES.items():
            with self.subTest(capture=name), tempfile.TemporaryDirectory(
                prefix="capture-partial-", dir=fixture_dir
            ) as scratch_name:
                runner, transport, cases = self._campaign(
                    Path(scratch_name), ("transport-write", case_id),
                    host0_log_contents={case_id: _console_capture(name)},
                )
                report = runner.run(cases, reset_between_cases=True, stop_on_incomplete=True)
                self.assertEqual(report["terminal_reason"], "PHYSICAL_INTERVENTION_REQUIRED")
                self.assertEqual(report["intervention_case_id"], case_id)
                self.assertIsNone(report["host_error"])


def _raise_on_second_call(real, error: Exception):
    """Wrap ``real`` so that its second call raises ``error`` (a synthetic host bug)."""

    calls = []

    def wrapper(*args, **kwargs):
        calls.append(args)
        if len(calls) == 2:
            raise error
        return real(*args, **kwargs)

    return wrapper


class HostErrorContainmentTests(unittest.TestCase):
    """A host-side exception ends the case by name, tears down, and checkpoints."""

    def _plan_run(self, scratch: Path, transport: SimulatedPsplinkTransport, *patches):
        helper = CampaignPlanCheckpointTests()
        plan_path = helper._plan(scratch)
        with contextlib.ExitStack() as stack:
            for target, attribute, kwargs in patches:
                stack.enter_context(patch.object(target, attribute, **kwargs))
            code, report = helper._run(plan_path, confirm=False, transport=transport)
        return code, report, helper._checkpoint(plan_path)

    @staticmethod
    def _alarm_transport(**options) -> SimulatedPsplinkTransport:
        return SimulatedPsplinkTransport(
            transport_file_cases={"transport-write"},
            host0_log_contents={
                "kernel-alarm": _console_capture("kernel_alarm.20261008-run3.txt")
            },
            **options,
        )

    def test_error_after_unload_with_clean_teardown_needs_no_power_cycle(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        crash = AttributeError("'str' object has no attribute 'status'")
        with tempfile.TemporaryDirectory(prefix="host-error-clean-", dir=fixture_dir) as name:
            transport = self._alarm_transport()
            code, report, checkpoint = self._plan_run(
                Path(name), transport,
                (PsplinkCampaignRunner, "_probe_case_succeeded", {"side_effect": [True, crash]}),
            )

        self.assertEqual(code, 2)
        self.assertEqual(report["terminal_reason"], "HOST_ERROR")
        self.assertIn("kernel-alarm: AttributeError", report["host_error"])
        self.assertIn("'str' object has no attribute 'status'", report["host_error"])
        envelope = report["envelopes"][-1]
        self.assertEqual(envelope["PROCESS_STATUS"], "HOST_ERROR")
        self.assertTrue(envelope["TEARDOWN_CHECK"]["post_error_teardown_clean"])
        self.assertEqual(checkpoint["state"], "IN_PROGRESS")
        self.assertEqual(checkpoint["next_case_index"], 2)
        self.assertEqual(checkpoint["completed_cases"], ["transport-write"])
        self.assertEqual(checkpoint["interrupted_cases"], ["kernel-alarm"])
        self.assertEqual(checkpoint["failed_case_id"], "kernel-alarm")

    def test_error_before_unload_still_unloads_the_probe(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        for fail_unload, expected_state, expected_code in (
            (False, "IN_PROGRESS", 2),
            (True, "WAITING_FOR_POWER_CYCLE", 3),
        ):
            with self.subTest(fail_unload=fail_unload), tempfile.TemporaryDirectory(
                prefix="host-error-unload-", dir=fixture_dir
            ) as name:
                transport = self._alarm_transport(
                    fail_modstun_cases={"kernel-alarm"} if fail_unload else set()
                )
                code, report, checkpoint = self._plan_run(
                    Path(name), transport,
                    (PsplinkCampaignRunner, "_module_threads", {
                        "autospec": True,
                        "side_effect": _raise_on_second_call(
                            PsplinkCampaignRunner._module_threads,
                            RuntimeError("synthetic host fault before unload"),
                        ),
                    }),
                )
                commands = [command for command, _timeout in transport.commands]
                launch = commands.index("ldstart host0:/kernel-alarm.prx")
                self.assertIn("modstun 0x04280001", commands[launch:])
                self.assertEqual(code, expected_code)
                self.assertIn("RuntimeError", report["host_error"])
                self.assertEqual(checkpoint["state"], expected_state)
                self.assertEqual(checkpoint["next_case_index"], 2)
                self.assertEqual(
                    report["envelopes"][-1]["TEARDOWN_CHECK"]["post_error_teardown_clean"],
                    not fail_unload,
                )

    def test_error_before_launch_keeps_the_case_queued(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        remote_path = _raise_on_second_call(
            run_psplink_module._host0_remote_path,
            RuntimeError("synthetic host fault before launch"),
        )
        with tempfile.TemporaryDirectory(prefix="host-error-prelaunch-", dir=fixture_dir) as name:
            transport = self._alarm_transport()
            code, report, checkpoint = self._plan_run(
                Path(name), transport,
                (run_psplink_module, "_host0_remote_path", {"side_effect": remote_path}),
            )
        commands = [command for command, _timeout in transport.commands]
        self.assertEqual(code, 2)
        self.assertEqual(report["terminal_reason"], "HOST_ERROR")
        self.assertNotIn("ldstart host0:/kernel-alarm.prx", commands)
        self.assertEqual(checkpoint["state"], "IN_PROGRESS")
        self.assertEqual(checkpoint["next_case_index"], 1)
        self.assertEqual(checkpoint["interrupted_cases"], [])

    def test_error_in_initial_qualification_is_named(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="host-error-qualify-", dir=fixture_dir) as name:
            code, report, checkpoint = self._plan_run(
                Path(name), self._alarm_transport(),
                (PsplinkCampaignRunner, "_qualify",
                 {"side_effect": KeyError("firmware")}),
            )
        self.assertEqual(code, 2)
        self.assertEqual(report["terminal_reason"], "HOST_ERROR")
        self.assertIn("before the first case: KeyError", report["host_error"])
        self.assertEqual((checkpoint["state"], checkpoint["next_case_index"]), ("IN_PROGRESS", 0))

    def test_campaign_plan_finalizes_the_checkpoint_if_the_runner_raises(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"

        def explode(runner, *args, **kwargs):
            kwargs["on_case_start"](0, CampaignCase("transport-write", Path("x.prx"), 1.0),
                                    "CASE_ACTIVE")
            raise ValueError("synthetic runner fault")

        with tempfile.TemporaryDirectory(prefix="host-error-plan-", dir=fixture_dir) as name:
            transport = self._alarm_transport()
            code, report, checkpoint = self._plan_run(
                Path(name), transport,
                (PsplinkCampaignRunner, "run", {"autospec": True, "side_effect": explode}),
            )
        self.assertEqual(code, 2)
        self.assertEqual(report["terminal_reason"], "HOST_ERROR")
        self.assertIn("campaign runner: ValueError", report["host_error"])
        # A launch was recorded without a completion: the durable state owes a power cycle.
        self.assertEqual(checkpoint["state"], "WAITING_FOR_POWER_CYCLE")
        self.assertEqual(checkpoint["next_case_index"], 0)
        self.assertTrue(transport.stopped)



def _registry_census_stream(categories: int = 44, keys: int = 365) -> str:
    """A registry-readonly stream with the 2026-10-08 console capture's shape.

    It mirrors the probe build that ran that night: one durable STEP marker
    per category, the fixed open/errors/bad-handle records, and a
    registry-done whose out2 counted only census records (categories + keys).
    The raw census itself (key names and modeled setting values) is console
    configuration and stays in the private campaign directory.
    """

    rows = [CAMPAIGN_META.rstrip("\n"),
            "NAKAGAWA_PSP_STEP schema=1 case_id=registry-readonly step=open-registry",
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 case_id=registry-open "
            "status=PASS result=0x00000000 out0=0x00000001 out1=0x00000001",
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 case_id=registry-errors "
            "status=PASS result=0x00000000 out0=0x00000000 out1=0x8008271d out2=0xffffffff "
            "out3=0x00000000 out4=0x00000100 out5=0x00000100 out6=0x00000000"]
    for index in range(categories):
        rows.append(
            "NAKAGAWA_PSP_STEP schema=1 case_id=registry-readonly "
            f"step=CONFIG/category{index:02d}"
        )
        rows.append(
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 "
            f"case_id=registry-category-{index:04d} status=PASS result=0x00000000 "
            f"out0=0x{(keys // categories):08x} out1=0x00000000 detail=CONFIG/category{index:02d}"
        )
    for index in range(keys):
        rows.append(
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 "
            f"case_id=registry-key-{index:04d} status=PASS result=0x00000000 "
            "out0=0x00000003 out1=0x00000004 out2=0x00000000 out3=0x00000000 "
            f"detail=CONFIG/category{index % categories:02d}/synthetic_key_{index:04d}"
        )
    rows += [
        "NAKAGAWA_PSP_STEP schema=1 case_id=registry-readonly step=bad-handle",
        "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 case_id=registry-bad-handle "
        "status=PASS result=0x8008272e out0=0x8008272e out1=0xffffffff",
        "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 case_id=registry-done "
        f"status=PASS result=0x00000000 out0=0x{categories:08x} out1=0x{keys:08x} "
        f"out2=0x{categories + keys:08x}",
        "NAKAGAWA_PSP_COMPLETE schema=1 status=PASS",
    ]
    return "\n".join(rows) + "\n"


class _SlowHost0Transport(SimulatedPsplinkTransport):
    """Writes one case's host0 log progressively after `ldstart`, like a slow probe."""

    def __init__(self, slow_case: str, stream: str, *, chunk: int = 40,
                 pause: float = 0.05, **options):
        super().__init__(host0_log_contents={slow_case: ""}, **options)
        self.slow_case = slow_case
        self.stream = stream
        self.chunk = chunk
        self.pause = pause
        self.writer: threading.Thread | None = None
        self.last_write_ns: int | None = None

    def run(self, command, timeout):
        result = super().run(command, timeout)
        if command == f"ldstart host0:/{self.slow_case}.prx":
            path = _campaign_host0_log_path(self.host0_root, self.slow_case)
            lines = self.stream.splitlines(keepends=True)

            def write() -> None:
                for start in range(0, len(lines), self.chunk):
                    with path.open("a", encoding="utf-8", newline="\n") as stream:
                        stream.write("".join(lines[start:start + self.chunk]))
                    self.last_write_ns = time.time_ns()
                    time.sleep(self.pause)

            self.writer = threading.Thread(target=write, daemon=True)
            self.writer.start()
        return result


class ProgressiveHost0StreamTests(unittest.TestCase):
    """The host0 wait ends at the probe's completion marker, bounded by the case timeout."""

    def test_wait_follows_a_growing_stream_until_its_completion_marker(self):
        stream = _registry_census_stream()
        lines = stream.splitlines(keepends=True)
        with tempfile.TemporaryDirectory() as scratch_name:
            path = Path(scratch_name) / "registry_readonly_log.txt"
            path.write_text("", encoding="utf-8")
            started_ns = time.time_ns()

            def write() -> None:
                for start in range(0, len(lines), 40):
                    with path.open("a", encoding="utf-8", newline="\n") as handle:
                        handle.write("".join(lines[start:start + 40]))
                    time.sleep(0.05)

            writer = threading.Thread(target=write, daemon=True)
            writer.start()
            text, _mtime = _wait_for_host0_output(
                path, 20.0, not_before_ns=started_ns - 10**9,
                ready=run_psplink_module._has_probe_completion_sentinel,
                include_mtime=True,
            )
            writer.join(5.0)
        self.assertEqual(text, stream)

    def test_wait_without_a_completion_marker_stops_at_the_case_timeout(self):
        with tempfile.TemporaryDirectory() as scratch_name:
            path = Path(scratch_name) / "registry_readonly_log.txt"
            path.write_text(_registry_census_stream().rsplit("NAKAGAWA_PSP_COMPLETE", 1)[0],
                            encoding="utf-8")
            started = time.monotonic()
            with self.assertRaises(TimeoutError):
                _wait_for_host0_output(
                    path, 0.5, ready=run_psplink_module._has_probe_completion_sentinel,
                )
            self.assertLess(time.monotonic() - started, 5.0)

    def test_progress_detail_names_records_last_write_and_last_step(self):
        partial = _registry_census_stream().split("step=bad-handle", 1)[0]
        detail = run_psplink_module._host0_progress_detail(partial, 3_500_000_000, 1_000_000_000)
        self.assertIn("411 result record(s)", detail)
        self.assertIn("last host0 write 2.5s after launch", detail)
        self.assertIn("last step marker: CONFIG/category43", detail)
        self.assertEqual(
            run_psplink_module._host0_progress_detail(None, None, 0),
            "no host0 output was observed",
        )

    def _run_registry(self, scratch: Path, timeout: float, **transport_options):
        cases = []
        for case_id, case_timeout in (("transport-write", 1.0), ("registry-readonly", timeout),
                                      ("smoke", 1.0)):
            binary = scratch / f"{case_id}.prx"
            binary.write_bytes(b"synthetic PRX")
            cases.append(CampaignCase(case_id, binary, case_timeout))
        transport = _SlowHost0Transport(
            "registry-readonly", _registry_census_stream(),
            transport_file_cases={"transport-write"},
            stdout_record_cases={"registry-readonly", "smoke"}, **transport_options,
        )
        transport.host0_root = scratch
        runner = PsplinkCampaignRunner(
            transport, console_model="PSP-3000-04g", source_commit=SOURCE_COMMIT, model_code=3,
        )
        started = time.monotonic()
        report = runner.run(cases, reset_between_cases=True, stop_on_incomplete=True)
        return report, transport, time.monotonic() - started

    def test_slow_finished_stream_is_captured_whole_and_never_unloaded_early(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="slow-registry-", dir=fixture_dir) as name:
            report, transport, elapsed = self._run_registry(Path(name), 60.0)
            transport.writer.join(5.0)

        commands = [command for command, _timeout in transport.commands]
        registry = report["envelopes"][1]
        # Every record reached host0 before the probe was unloaded ...
        self.assertEqual(transport.host0_record_counts_at_unload["registry-readonly"], 413)
        self.assertIn("case_id=registry-done", registry["RAW_RESULT"])
        # ... and the wait ended at the completion marker, far inside the 60 s budget.
        self.assertLess(elapsed, 30.0)
        # The finished stream violates its record contract (on 2026-10-08, the
        # registry-done count), which is a named protocol failure with a normal
        # teardown, not an incomplete case that demands a power cycle.
        self.assertIsNone(report["terminal_reason"])
        self.assertEqual(registry["TEARDOWN_CHECK"]["status"], "PASS")
        self.assertEqual(registry["QUALIFICATION_STATUS"], "UNQUALIFIED")
        self.assertTrue(any(
            "strict protocol validation" in blocker
            for blocker in registry["QUALIFICATION_BLOCKERS"]
        ))
        self.assertIn("ldstart host0:/smoke.prx", commands)

    def test_unfinished_slow_stream_names_its_progress_and_stops(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        unfinished = _registry_census_stream().rsplit("NAKAGAWA_PSP_COMPLETE", 1)[0]
        with tempfile.TemporaryDirectory(prefix="slow-unfinished-", dir=fixture_dir) as name:
            scratch = Path(name)
            cases = []
            for case_id, case_timeout in (("transport-write", 1.0), ("registry-readonly", 1.5)):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(b"synthetic PRX")
                cases.append(CampaignCase(case_id, binary, case_timeout))
            transport = _SlowHost0Transport(
                "registry-readonly", unfinished, transport_file_cases={"transport-write"},
            )
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport, console_model="PSP-3000-04g", source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run(cases, reset_between_cases=True, stop_on_incomplete=True)
            transport.writer.join(5.0)

        registry = report["envelopes"][1]
        self.assertEqual(report["terminal_reason"], "PHYSICAL_INTERVENTION_REQUIRED")
        self.assertEqual(report["intervention_case_id"], "registry-readonly")
        problem = " ".join(registry["QUALIFICATION_BLOCKERS"])
        self.assertIn("reached no completion marker within the 1.5s case timeout", problem)
        self.assertIn("last step marker:", problem)


class RealProbeContractTests(unittest.TestCase):
    """Fixed contracts follow what the probes really emit (2026-10-08 run 5)."""

    DISPLAY = "display_wait_late.20261008-run5.txt"
    MUTEX = "mutex_interrupt_context.20261008-run5.txt"

    @staticmethod
    def _records(text: str) -> str:
        return text.split("NAKAGAWA_PSP_COMPLETE", 1)[0]

    def test_display_wait_late_contract_is_the_probe_emission_order(self):
        text = _console_capture(self.DISPLAY)
        parsed = _parse_campaign_records(self._records(text), "display-wait-late")
        self.assertEqual(
            [record.case_id for record in parsed.results],
            ["calibration"]
            + [f"late-waitvblankstart-{n}eighths" for n in (2, 6, 10, 14, 20)]
            + ["invblank-waitvblankstart"]
            + [f"late-waitvblank-{n}eighths" for n in (2, 6, 10, 14, 20)]
            + ["invblank-waitvblank"],
        )
        # The previous contract grouped both APIs' late cells before either
        # in-vblank cell; the probe never emits that order.
        lines = self._records(text).splitlines(keepends=True)
        start_invblank = next(i for i, line in enumerate(lines)
                              if "case_id=invblank-waitvblankstart " in line)
        regrouped = lines[:start_invblank] + lines[start_invblank + 1:-1] + \
            [lines[start_invblank], lines[-1]]
        with self.assertRaises(PspProtocolError):
            _parse_campaign_records("".join(regrouped), "display-wait-late")

    def test_mutex_interrupt_trials_follow_their_recorded_interrupt_state(self):
        text = _console_capture(self.MUTEX)
        self.assertTrue(run_psplink_module._campaign_stream_complete(text, "mutex-interrupt-context"))
        parsed = _parse_campaign_records(self._records(text), "mutex-interrupt-context")
        trials = [record for record in parsed.results if record.case_id.endswith(tuple(
            f"-t{index:02d}" for index in range(20)))]
        self.assertEqual(len(trials), 20)
        for record in trials:
            values = dict(record.values)
            # PSP-3000 / 6.6.1: every VBLANK handler trial saw interrupts enabled,
            # so by the probe's design none counts; the mutex returns stay recorded.
            self.assertEqual((record.status, values["out0"]), ("FAIL", "0x00000001"))
            self.assertEqual(values["out1"], "0x80020064")
        for forged in (
            # A trial claiming to count while it recorded interrupts enabled.
            self._records(text).replace(
                "case_id=mutex-interrupt-context-t07 status=FAIL result=0x00000000",
                "case_id=mutex-interrupt-context-t07 status=PASS result=0x00000001", 1),
            # A trial that recorded interrupts disabled but was not counted.
            self._records(text).replace(
                "case_id=mutex-interrupt-context-t03 status=FAIL result=0x00000000 out0=0x00000001",
                "case_id=mutex-interrupt-context-t03 status=FAIL result=0x00000000 out0=0x00000000",
                1),
        ):
            with self.assertRaises(PspProtocolError):
                _parse_campaign_records(forged, "mutex-interrupt-context")

    def test_mutex_interrupt_capture_is_qualified_but_not_eligible(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="capture-mutex-", dir=fixture_dir) as name:
            runner, _transport, cases = ConsoleCaptureRegressionTests()._campaign(
                Path(name), ("transport-write", "mutex-interrupt-context"),
                host0_log_contents={"mutex-interrupt-context": _console_capture(self.MUTEX)},
                stdout_record_cases={"mutex-interrupt-context"},
            )
            report = runner.run(cases, reset_between_cases=True, stop_on_incomplete=True)

        envelope = report["envelopes"][1]
        self.assertIsNone(report["terminal_reason"])
        self.assertEqual(envelope["TEARDOWN_CHECK"]["status"], "PASS")
        self.assertFalse(any(
            "strict protocol validation" in blocker
            for blocker in envelope["QUALIFICATION_BLOCKERS"]
        ))
        self.assertFalse(envelope["ACCEPTANCE_ELIGIBLE"])
        self.assertIn("one or more scalar result records did not pass",
                      envelope["ACCEPTANCE_BLOCKERS"])


class Host0RemotePathTests(unittest.TestCase):
    """A campaign PRX staged in a subdirectory is loaded by its host0-relative path."""

    def test_subdirectory_prx_keeps_its_relative_path(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            prx = root / "ge-nan" / "nakagawa_psp_oracle.prx"
            prx.parent.mkdir()
            prx.write_bytes(b"\x00")
            self.assertEqual(run_psplink_module._host0_remote_path(prx, root),
                             "ge-nan/nakagawa_psp_oracle.prx")
            flat = root / "fpu-vector.prx"
            flat.write_bytes(b"\x00")
            self.assertEqual(run_psplink_module._host0_remote_path(flat, root), "fpu-vector.prx")

    def test_without_a_host0_root_the_bare_name_is_used(self) -> None:
        self.assertEqual(run_psplink_module._host0_remote_path(Path("x/y.prx"), None), "y.prx")

    def test_a_campaign_prx_outside_the_root_stops_with_a_report(self) -> None:
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-outside-root-", dir=fixture_dir) as scratch_name, \
                tempfile.TemporaryDirectory() as other_name:
            scratch = Path(scratch_name)
            inside = scratch / "transport-write.prx"
            inside.write_bytes(b"synthetic PRX")
            outside = Path(other_name) / "fpu-vector.prx"
            outside.write_bytes(b"synthetic PRX")
            transport = SimulatedPsplinkTransport()
            transport.host0_root = scratch
            report = PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run([CampaignCase("transport-write", inside, 1.0),
                   CampaignCase("fpu-vector", outside, 1.0)])
        commands = [command for command, _timeout in transport.commands]
        self.assertEqual(report["state"], "STOPPED")
        self.assertEqual(report["terminal_reason"], "HOST0_PRX_OUTSIDE_ROOT")
        self.assertFalse([c for c in commands if c.startswith("ldstart") and "fpu-vector" in c])
        blockers = " ".join(report["envelopes"][-1]["QUALIFICATION_BLOCKERS"])
        self.assertIn("campaign PRX for case fpu-vector is not inside host0 root", blockers)
        self.assertNotIn(other_name, blockers)
        self.assertNotIn(scratch_name, blockers)

    def test_a_prx_outside_the_root_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as root, tempfile.TemporaryDirectory() as other:
            outside = Path(other) / "probe.prx"
            outside.write_bytes(b"\x00")
            with self.assertRaises(ValueError):
                run_psplink_module._host0_remote_path(outside, Path(root))


class _HleFamilyTransport(SimulatedPsplinkTransport):
    """A simulated HLE launch whose finish leaves the host0 round-trip file, or does not.

    probe.c's teardown and the HLE families' hle_finish both write
    host0:/nakagawa_transport_write.bin. Only the transport-write case gets the file from
    the base class, so an HLE launch writes exactly what its own finish would.
    """

    def __init__(self, roundtrip: bytes | None):
        super().__init__(transport_file_cases={"transport-write"})
        self.roundtrip = roundtrip

    def run(self, command, timeout):
        if (
            self.roundtrip is not None
            and command.startswith("ldstart host0:/hle-")
            and self.host0_root is not None
        ):
            (self.host0_root / "nakagawa_transport_write.bin").write_bytes(self.roundtrip)
        return super().run(command, timeout)


class HleFamilyHost0RoundTripTests(unittest.TestCase):
    """An HLE family's launch must leave the host0 round-trip file the runner verifies."""

    PROBE_HLE_SOURCE = (
        Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle" / "probe_hle_measure.c"
    )
    PATTERN = "(uint8_t)(0x5Au ^ (i * 0x25u + (i >> 3)))"

    def _launch(self, roundtrip: bytes | None):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(
            prefix="runner-hle-roundtrip-", dir=fixture_dir
        ) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id in ("transport-write", "hle-kernel-status"):
                binary = scratch / f"{case_id}.prx"
                binary.write_bytes(b"synthetic PRX")
                cases.append(CampaignCase(case_id, binary, 1.0))
            transport = _HleFamilyTransport(roundtrip)
            transport.host0_root = scratch
            with patch("psp_oracle.run_psplink._check_source_tree", return_value=None):
                report = PsplinkCampaignRunner(
                    transport,
                    console_model="PSP-3000-04g",
                    source_commit=SOURCE_COMMIT,
                    model_code=3,
                ).run(cases)
            left_behind = (scratch / "nakagawa_transport_write.bin").exists()
        return report, left_behind

    def test_round_trip_file_written_per_the_family_contract_passes_verification(self):
        source = self.PROBE_HLE_SOURCE.read_text(encoding="utf-8")
        self.assertTrue(self.PATTERN in source,
                        "probe_hle_measure.c must write the round-trip byte pattern")
        # The 64 bytes the family's expression produces, as _verify_host0_roundtrip expects.
        roundtrip = bytes((0x5A ^ (index * 0x25 + (index >> 3))) & 0xFF for index in range(64))
        report, left_behind = self._launch(roundtrip)

        envelope = report["envelopes"][1]
        self.assertEqual(envelope["CASE_ID"], "hle-kernel-status")
        self.assertNotEqual(report["terminal_reason"], "HOST0_ROUNDTRIP_FAILED")
        round_trip_issues = [
            issue for issue in envelope["TEARDOWN_CHECK"]["issues"] if "host0 round-trip" in issue
        ]
        self.assertEqual(round_trip_issues, [])
        self.assertFalse(left_behind)  # the runner verified the file and removed it

    def test_family_without_the_round_trip_file_fails_the_runner_check(self):
        report, _left_behind = self._launch(None)

        envelope = report["envelopes"][1]
        self.assertEqual(report["terminal_reason"], "HOST0_ROUNDTRIP_FAILED")
        self.assertIn("host0 round-trip failed after unload",
                      envelope["TEARDOWN_CHECK"]["issues"])
        self.assertFalse(envelope["ACCEPTANCE_ELIGIBLE"])


class _PostUnloadLinkTransport(SimulatedPsplinkTransport):
    """After each `modstun`, `ver` answers only from attempt ``answer_on`` on.

    Models the hardware settle race: the case completes and the stop/unload
    handshake succeeds, then the link loses `ver` replies for a while.
    ``answer_on=None`` never answers `ver` again after the unload.
    """

    def __init__(self, answer_on: int | None, **options):
        super().__init__(**options)
        self.answer_on = answer_on
        self.post_unload_ver_attempts: int | None = None

    def run(self, command, timeout):
        if command == f"modstun {self._probe_uid}":
            self.post_unload_ver_attempts = 0
        elif command == "ver" and self.post_unload_ver_attempts is not None:
            self.post_unload_ver_attempts += 1
            if self.answer_on is None or self.post_unload_ver_attempts < self.answer_on:
                self.commands.append((command, timeout))
                return None, "", "", "TIMEOUT"
        return super().run(command, timeout)


# The post-launch command sequence of one passing case before the settle existed:
# S1, the module thread query, the stop/unload handshake, S2, the shell
# qualification and the exception query.
_LAUNCH = "ldstart host0:/transport-write.prx"
_UNLOAD_HANDSHAKE = ["modstun 0x04280001", "modinfo 0x04280001"]
_PRE_UNLOAD = [_LAUNCH, "thlist", "meminfo", "modlist", "modinfo 0x04280001 t"]
_POST_SETTLE = ["thlist", "meminfo", "modlist", "ver", "usbstat", "pwd", "exprint"]


class PostUnloadSettleTests(unittest.TestCase):
    """The link settles after the unload handshake, before S2, qualification and round-trip."""

    def setUp(self):
        source_check = patch("psp_oracle.run_psplink._check_source_tree", return_value=None)
        source_check.start()
        self.addCleanup(source_check.stop)

    def _run_transport_write(self, transport, **run_options):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-settle-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            binary = scratch / "transport-write.prx"
            binary.write_bytes(b"synthetic PRX")
            transport.host0_root = scratch
            return PsplinkCampaignRunner(
                transport,
                console_model="PSP-3000-04g",
                source_commit=SOURCE_COMMIT,
                model_code=3,
            ).run([CampaignCase("transport-write", binary, 1.0)], **run_options)

    @staticmethod
    def _after_launch(transport) -> list[str]:
        commands = [command for command, _timeout in transport.commands]
        return commands[commands.index(_LAUNCH):]

    @staticmethod
    def _classification(report) -> dict[str, object]:
        envelope = report["envelopes"][-1]
        teardown = envelope["TEARDOWN_CHECK"]
        return {
            "state": report["state"],
            "terminal_reason": report["terminal_reason"],
            "intervention_case_id": report["intervention_case_id"],
            "resume_case_index": report["resume_case_index"],
            "teardown_status": teardown["status"],
            "teardown_issues": teardown["issues"],
            "recovery_eligible": teardown["recovery_eligible"],
            "recovery_status": teardown["recovery_status"],
            "shell_qualified": teardown["shell_qualified"],
            "qualification_status": envelope["QUALIFICATION_STATUS"],
            "qualification_blockers": envelope["QUALIFICATION_BLOCKERS"],
            "session_qualification_status": envelope["SESSION_QUALIFICATION_STATUS"],
            "acceptance_eligible": envelope["ACCEPTANCE_ELIGIBLE"],
            "acceptance_blockers": envelope["ACCEPTANCE_BLOCKERS"],
            "evidence_class": envelope["EVIDENCE_CLASS"],
        }

    def test_ver_answering_on_the_third_attempt_after_unload_passes_with_three_settle_attempts(self):
        transport = _PostUnloadLinkTransport(answer_on=3)
        report = self._run_transport_write(transport)

        envelope = report["envelopes"][0]
        teardown = envelope["TEARDOWN_CHECK"]
        self.assertIsNone(report["terminal_reason"])
        self.assertEqual(teardown["status"], "PASS")
        self.assertEqual(teardown["settle_status"], "PASS")
        self.assertEqual(teardown["settle_attempts"], 3)
        self.assertTrue(teardown["shell_qualified"])
        self.assertTrue(envelope["ACCEPTANCE_ELIGIBLE"])
        settle_events = [
            event for event in report["recovery_events"]
            if event.startswith("post-unload settle: shell verification attempt ")
        ]
        self.assertEqual(len(settle_events), 3)
        self.assertIn("reply timed out or was lost", settle_events[0])
        self.assertIn("reply timed out or was lost", settle_events[1])
        self.assertTrue(settle_events[2].endswith("attempt 3/3: PASS"))
        # Settle attempts never count as shell qualification attempts: the
        # initial and the post-unload qualification each passed on their first `ver`.
        qualification_events = [
            event for event in report["recovery_events"]
            if event.startswith("shell verification attempt ")
        ]
        self.assertEqual(qualification_events, ["shell verification attempt 1/3: PASS"] * 2)

    def test_a_settle_that_never_answers_keeps_todays_failure_classification(self):
        for mode, run_options in (
            ("campaign-case", {}),
            ("campaign-plan", {"reset_between_cases": True, "stop_on_incomplete": True}),
        ):
            with self.subTest(mode=mode):
                transport = _PostUnloadLinkTransport(answer_on=None)
                report = self._run_transport_write(transport, **run_options)
                # Today's runner: the same link without the settle step.
                before = _PostUnloadLinkTransport(answer_on=None)
                with patch.object(
                    PsplinkCampaignRunner, "_settle_after_unload", return_value=("NOT_RUN", 0)
                ):
                    before_report = self._run_transport_write(before, **run_options)

                teardown = report["envelopes"][0]["TEARDOWN_CHECK"]
                self.assertEqual(teardown["settle_status"], "EXHAUSTED")
                self.assertEqual(teardown["settle_attempts"], 3)
                self.assertEqual(teardown["status"], "FAIL")
                self.assertIn("PSPLink shell is not qualified after unload", teardown["issues"])
                self.assertFalse(report["envelopes"][0]["ACCEPTANCE_ELIGIBLE"])
                self.assertEqual(
                    report["terminal_reason"],
                    "TEARDOWN_RECOVERY_NOT_ELIGIBLE" if mode == "campaign-case"
                    else "PHYSICAL_INTERVENTION_REQUIRED",
                )
                self.assertEqual(self._classification(report), self._classification(before_report))
                # The exhausted settle adds its three `ver` attempts and nothing else.
                settle_index = len(_PRE_UNLOAD) + len(_UNLOAD_HANDSHAKE)
                commands = self._after_launch(transport)
                self.assertEqual(commands[settle_index:settle_index + 3], ["ver"] * 3)
                del commands[settle_index:settle_index + 3]
                self.assertEqual(commands, self._after_launch(before))

    def test_settle_commands_sit_between_the_unload_handshake_and_s2(self):
        transport = _PostUnloadLinkTransport(answer_on=3)
        self._run_transport_write(transport)
        self.assertEqual(
            self._after_launch(transport),
            _PRE_UNLOAD + _UNLOAD_HANDSHAKE + ["ver", "ver", "ver"] + _POST_SETTLE,
        )

        # Every other command keeps its count and order: on a link that answers at
        # once, removing the single settle `ver` leaves today's sequence.
        settled = SimulatedPsplinkTransport()
        self._run_transport_write(settled)
        today = SimulatedPsplinkTransport()
        with patch.object(
            PsplinkCampaignRunner, "_settle_after_unload", return_value=("NOT_RUN", 0)
        ):
            self._run_transport_write(today)
        settle_index = len(_PRE_UNLOAD) + len(_UNLOAD_HANDSHAKE)
        settled_commands = self._after_launch(settled)
        self.assertEqual(settled_commands[settle_index], "ver")
        del settled_commands[settle_index]
        self.assertEqual(settled_commands, self._after_launch(today))
        self.assertEqual(self._after_launch(today), _PRE_UNLOAD + _UNLOAD_HANDSHAKE + _POST_SETTLE)

    def test_no_settle_without_an_unload_handshake_or_after_the_session_stopped(self):
        class NoModuleUidTransport(SimulatedPsplinkTransport):
            def run(self, command, timeout):
                returncode, stdout, stderr, status = super().run(command, timeout)
                if command.startswith("ldstart "):
                    stdout = stdout.replace(f"UID: {self._probe_uid}", "UID: none")
                return returncode, stdout, stderr, status

        transport = NoModuleUidTransport()
        report = self._run_transport_write(transport)
        teardown = report["envelopes"][0]["TEARDOWN_CHECK"]
        self.assertEqual(teardown["settle_status"], "NOT_RUN")
        self.assertEqual(teardown["settle_attempts"], 0)
        self.assertEqual(
            self._after_launch(transport),
            [_LAUNCH, "thlist", "meminfo", "modlist"] + _POST_SETTLE,
        )

        stopped = SimulatedPsplinkTransport()
        runner = PsplinkCampaignRunner(
            stopped, console_model="PSP-3000-04g", source_commit=SOURCE_COMMIT
        )
        runner.terminal_reason = "TRANSPORT_RESULT_DISCARDED"
        self.assertEqual(runner._settle_after_unload(), ("NOT_RUN", 0))
        self.assertEqual(stopped.commands, [])
        self.assertEqual(runner.recovery_events, [])


if __name__ == "__main__":
    unittest.main()
