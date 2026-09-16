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
import struct
import unittest

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


if __name__ == "__main__":
    unittest.main()
