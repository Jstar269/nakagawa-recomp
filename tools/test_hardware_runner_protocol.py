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
* raw device values (including the recorded PSP-3000-series vs raw-model-3
  contradiction) are preserved verbatim next to interpreted labels.
"""

from __future__ import annotations

import hashlib
from io import StringIO
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
    CampaignCase,
    PsplinkCampaignRunner,
    _campaign_host0_log_path,
    _parse_campaign_records,
    _parse_usbipd_psplink_devices,
    _verify_psplink_shell,
    PsplinkProcessTransport,
    _run_command,
)

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


class SimulatedPsplinkTransport:
    """Command-level fake for the real PSPSH adapter; never touches hardware."""

    def __init__(
        self,
        *,
        timeout_cases: set[str] | None = None,
        fail_modstun: bool = False,
        fail_post_case_ver_once: bool = False,
        fail_first_ver_once: bool = False,
        fail_all_ver: bool = False,
        unknown_command_ver_once: bool = False,
        reset_timeout: bool = False,
        reset_returncode: int = 0,
        stdout_record_cases: set[str] | None = None,
        stdout_result_overrides: dict[str, str] | None = None,
        write_host0_logs: bool = True,
        stale_host0_mtime: bool = False,
    ):
        self.timeout_cases = timeout_cases or set()
        self.fail_modstun = fail_modstun
        self.fail_post_case_ver_once = fail_post_case_ver_once
        self.fail_first_ver_once = fail_first_ver_once
        self.fail_all_ver = fail_all_ver
        self.unknown_command_ver_once = unknown_command_ver_once
        self.first_ver_failed = False
        self.reset_timeout = reset_timeout
        self.reset_returncode = reset_returncode
        self.stdout_record_cases = stdout_record_cases or set()
        self.stdout_result_overrides = stdout_result_overrides or {}
        self.write_host0_logs = write_host0_logs
        self.stale_host0_mtime = stale_host0_mtime
        self.post_case_ver_failed = False
        self.case_started = False
        self.started = False
        self.stopped = False
        self.restarts = 0
        self.commands: list[tuple[str, float]] = []
        self.host0_root: Path | None = None
        self.stale_log_present_at_load = False
        self.current_case = ""
        self.waiting_for_device = False
        self.transport_recoveries = 0
        self.unknown_command_events = 0

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
        if command.startswith("ldstart host0:/"):
            case_id = Path(command).name.removesuffix(".prx")
            self.current_case = case_id
            self.case_started = True
            if case_id in self.timeout_cases:
                return None, "Load/Start UID: 0x04280001\nNAKAGAWA_PSP_TEST partial", "", "TIMEOUT"
            if case_id == "transport-write" and self.host0_root is not None:
                pattern = bytes(
                    (0x5A ^ (index * 0x25 + (index >> 3))) & 0xFF
                    for index in range(64)
                )
                (self.host0_root / "nakagawa_transport_write.bin").write_bytes(pattern)
            log_stem = case_id.replace("-", "_")
            if log_stem.startswith("dma_"):
                log_stem = "dmac_" + log_stem[4:]
            host0_log = self.host0_root / f"{log_stem}_log.txt" if self.host0_root else None
            self.stale_log_present_at_load = bool(host0_log and host0_log.exists())
            metadata_record = (
                "NAKAGAWA_PSP_META schema=1 source=psp model=fixture firmware=test "
                "binary_sha256=" + "0" * 64 + " source_commit=" + SOURCE_COMMIT + "\n"
            )
            if case_id == "transport-write":
                result_record = (
                    "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-TRANSPORT-001 "
                    "case_id=host0-write-readback status=PASS result=0x0\n"
                )
            else:
                result_record = (
                    "NAKAGAWA_PSP_TEST schema=1 test_id=SYNTHETIC case_id=" + case_id
                    + " status=PASS result=0x1\n"
                )
            stdout_record = result_record
            if case_id in self.stdout_result_overrides and case_id != "transport-write":
                stdout_record = (
                    "NAKAGAWA_PSP_TEST schema=1 test_id=SYNTHETIC case_id=" + case_id
                    + " status=PASS result=" + self.stdout_result_overrides[case_id] + "\n"
                )
            if self.write_host0_logs and self.host0_root is not None:
                assert host0_log is not None
                host0_log.write_text(
                    metadata_record + result_record, encoding="utf-8"
                )
                if self.stale_host0_mtime:
                    os.utime(host0_log, ns=(1, 1))
            stdout_records = "" if case_id in self.stdout_record_cases else metadata_record + stdout_record
            return 0, (
                "Load/Start host0:/" + case_id + ".prx UID: 0x04280001\n"
                + stdout_records
            ), "", "PROCESS_EXITED"
        if command == "modstun 0x04280001":
            if self.fail_modstun and self.current_case != "transport-write":
                return 1, "Module Stop/Unload failed\n", "", "PROCESS_EXITED"
            return 0, "Module Stop/Unload 0x00000000/0x04280001 Status 0xDEADBEEF\n", "", "PROCESS_EXITED"
        if command == "modinfo 0x04280001":
            return 1, "ERROR: Unknown module 0x04280001\n", "", "PROCESS_EXITED"
        if command == "reset" and self.reset_timeout:
            return None, "", "", "TIMEOUT"
        if command == "reset":
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
            "PHYSICAL_MODEL_LABEL": self.meta.get("label", "unset"),
            "SOFTWARE_MODEL_RAW_VALUE": self.meta.get("raw_model", "unset"),
            "MODEL_CONTRADICTION": "RECORDED"
            if self.meta.get("label", "").startswith("psp-3000")
            and self.meta.get("raw_model") == "3"
            else "NONE",
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

    # -- 14. model contradiction is preserved, never silently corrected -
    def test_14_model_contradiction_recorded_not_corrected(self):
        orch, _, _ = self.build()
        orch.qualify()
        orch.run_case("meta_probe")
        env = orch.envelopes[-1]
        self.assertEqual(env["SOFTWARE_MODEL_RAW_VALUE"], "3")
        self.assertEqual(env["PHYSICAL_MODEL_LABEL"], "psp-3000-series")
        self.assertEqual(env["MODEL_CONTRADICTION"], "RECORDED")

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
            for case_id in ("transport-write", "probe_a", "probe_b"):
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
            ["transport-write", "probe_a", "probe_b"],
        )
        for envelope in report["envelopes"]:
            self.assertEqual(envelope["CONSOLE_MODEL"], "PSP-3000-04g")
            self.assertEqual(envelope["FW"], "6.6.1")
            self.assertEqual(envelope["SOFTWARE_MODEL_RAW_VALUE"], "3")
            self.assertTrue(envelope["ACCEPTANCE_ELIGIBLE"])
            self.assertNotIn("SERIAL", " ".join(envelope).upper())
        commands = [command for command, _timeout in transport.commands]
        self.assertEqual(commands.count("modstun 0x04280001"), 3)
        self.assertEqual(commands.count("modinfo 0x04280001"), 3)
        self.assertEqual(
            [timeout for command, timeout in transport.commands if command.startswith("ldstart")],
            [1.25, 1.25, 1.25],
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
            for case_id in ("transport-write", "timeout", "good"):
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

        _transport, timed_out, succeeded = report["envelopes"]
        self.assertEqual(timed_out["PROCESS_STATUS"], "TIMEOUT")
        self.assertFalse(timed_out["ACCEPTANCE_ELIGIBLE"])
        self.assertEqual(timed_out["RAW_RESULT"], "")
        self.assertTrue(succeeded["ACCEPTANCE_ELIGIBLE"])

    def test_19_real_adapter_attempts_l0_l1_l2_then_stops_at_physical_intervention(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-escalation-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            transport_write = scratch / "transport-write.prx"
            transport_write.write_bytes(b"synthetic transport PRX")
            binary = scratch / "probe.prx"
            binary.write_bytes(b"synthetic PRX")
            transport = SimulatedPsplinkTransport(
                fail_modstun=True,
                fail_post_case_ver_once=True,
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
                CampaignCase("probe", binary, 0.5),
            ])

        self.assertEqual(transport.restarts, 1)
        self.assertEqual(report["terminal_reason"], "PHYSICAL_INTERVENTION_REQUIRED")
        self.assertIn("L0:", " ".join(report["recovery_events"]))
        self.assertIn("L1:", " ".join(report["recovery_events"]))
        self.assertIn("L2:", " ".join(report["recovery_events"]))
        self.assertIn("L4:", " ".join(report["recovery_events"]))
        self.assertIn("reset", [command for command, _ in transport.commands])

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

    def test_24_qualified_cleanup_failure_uses_l1_before_l2_reset(self):
        fixture_dir = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="runner-ladder-order-", dir=fixture_dir) as scratch_name:
            scratch = Path(scratch_name)
            cases = []
            for case_id in ("transport-write", "probe"):
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
        self.assertLess(next(i for i, event in enumerate(events) if event.startswith("L1:")),
                        next(i for i, event in enumerate(events) if event.startswith("L2:")))
        self.assertEqual(transport.restarts, 1)

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
        self.assertEqual(len(ver_events), 2)
        self.assertIn("reply timed out or was lost", ver_events[0])
        self.assertIn("PASS", ver_events[1])
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


if __name__ == "__main__":
    unittest.main()
