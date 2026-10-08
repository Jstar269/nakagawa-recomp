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

import hashlib
from io import StringIO
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
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
    _parse_usbipd_psplink_devices,
    _snapshot_host0_output,
    _wait_for_host0_output,
    _verify_psplink_shell,
    PsplinkProcessTransport,
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
    ):
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

    def start(self) -> None:
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
            complete_host0_log = (
                metadata_record
                + result_record
                + "NAKAGAWA_PSP_COMPLETE schema=1 status=PASS\n"
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
                "did not become complete before probe unload" in blocker
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

    def test_20_process_transport_uses_argv_templates_timeout_and_owned_server_lifecycle(self):
        class FakeProcess:
            def __init__(self):
                self.stdout = StringIO("")
                self.terminated = False
                self.killed = False

            def poll(self):
                return 0 if self.terminated or self.killed else None

            def terminate(self):
                self.terminated = True

            def wait(self, timeout):
                return 0

            def kill(self):
                self.killed = True

        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-process-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            created = []
            popen_calls = []
            command_calls = []

            def popen_factory(command, **kwargs):
                popen_calls.append((command, kwargs))
                process = FakeProcess()
                created.append(process)
                return process

            def command_runner(command, timeout):
                command_calls.append((command, timeout))
                return 0, "ok", "", "PROCESS_EXITED"

            adapter = PsplinkProcessTransport(
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

        self.assertEqual(command_calls, [(["fake-pspsh", "-e", "ldstart host0:/probe.prx"], 0.5)])
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
        self.assertIn("did not become complete", " ".join(envelope["TEARDOWN_CHECK"]["issues"]))
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

    def test_shell_qualification_exhaustion_requires_physical_intervention(self):
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
        self.assertEqual(report["terminal_reason"], "PHYSICAL_INTERVENTION_REQUIRED")
        self.assertEqual(len(attempts), 3)
        self.assertIn("usbipd list", report["recovery_events"][-1])
        self.assertIn("pspsh -e ver", report["recovery_events"][-1])
        self.assertEqual(report["envelopes"], [])

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

    def test_27_shared_psplink_device_is_attached_and_verified(self):
        class FakeProcess:
            def __init__(self):
                self.stdout = StringIO("Waiting for device...\n")
                self.terminated = False

            def poll(self):
                return 0 if self.terminated else None

            def terminate(self):
                self.terminated = True

            def wait(self, timeout):
                return 0

            def kill(self):
                self.terminated = True

        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="usbipd-reattach-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            process = FakeProcess()
            calls = []
            shell_events = []
            ver_attempts = 0
            adapter = None

            def command_runner(command, timeout):
                calls.append(command)
                if command == ["usbipd", "list"]:
                    return (
                        0,
                        "Connected:\nBUSID VID:PID DEVICE STATE\n"
                        "9-7.2 054c:01c9 PSP Type B Shared\n",
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
                pspsh_argv=["fake-pspsh", "-e", "{remote_command}"],
                usbhostfs_argv=["fake-usbhostfs", "{host0_root}"],
                host0_root=scratch,
                command_runner=command_runner,
                popen_factory=lambda _command, **_kwargs: process,
            )
            adapter.start()
            self.assertTrue(adapter._waiting_for_device.wait(0.5))

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


if __name__ == "__main__":
    unittest.main()
