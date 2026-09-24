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
import subprocess
import sys
import threading
import time
from typing import Callable

try:
    from .protocol import (
        ProtocolError,
        compare_texts,
        decode_psp_model_code,
        dump_json,
        parse_output,
        provenance_issues,
        validate_dmac_size_matrix,
    )
except ImportError:  # direct ``python tools/psp_oracle/run_psplink.py`` invocation
    from protocol import (  # type: ignore
        ProtocolError,
        compare_texts,
        decode_psp_model_code,
        dump_json,
        parse_output,
        provenance_issues,
        validate_dmac_size_matrix,
    )


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_RESULTS = ROOT / "oracle" / "hardware-results"
TERMINAL_OUTCOMES = frozenset({"HANG", "RESET"})
_TEST_RECORD_RE = re.compile(
    r"^NAKAGAWA_PSP_TEST\b.*\bstatus=([A-Z]+)\b", re.MULTILINE
)
SHELL_VERIFICATION_ATTEMPTS = 3
SHELL_VERIFICATION_ATTEMPT_TIMEOUT = 15.0
DEFAULT_SHELL_VERIFICATION_TIMEOUT = 45.0


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


def _wsl_path(path: Path) -> str:
    """Convert a Windows path to the standard WSL /mnt/<drive> form."""

    drive, tail = os.path.splitdrive(str(path))
    if not drive:
        return path.as_posix()
    return f"/mnt/{drive[0].lower()}/{tail.lstrip('\\/').replace('\\', '/')}"


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


class PsplinkProcessTransport:
    """Real host process adapter for one standalone PSPLINK/USBHostFS route."""

    def __init__(
        self,
        *,
        pspsh_argv: list[str],
        usbhostfs_argv: list[str],
        host0_root: Path,
        usbipd_argv: list[str] | None = None,
        command_runner: Callable[[list[str], float], tuple[int | None, str, str, str]] = _run_command,
        popen_factory: Callable[..., subprocess.Popen] = subprocess.Popen,
    ) -> None:
        self.pspsh_argv = list(pspsh_argv)
        self.usbhostfs_argv = list(usbhostfs_argv)
        self.usbipd_argv = list(usbipd_argv or ["usbipd"])
        self.host0_root = host0_root.resolve()
        self.command_runner = command_runner
        self.popen_factory = popen_factory
        self.server: subprocess.Popen | None = None
        self._server_output_thread: threading.Thread | None = None
        self._waiting_for_device = threading.Event()
        self._connected_to_device = threading.Event()
        self._server_output_lock = threading.Lock()
        self._unknown_command_events = 0

    def _server_argv(self) -> list[str]:
        return _render_argv(
            self.usbhostfs_argv,
            host0_root=str(self.host0_root),
            host0_root_wsl=_wsl_path(self.host0_root),
        )

    def start(self) -> None:
        if not self.host0_root.is_dir():
            raise FileNotFoundError("host0 root must be an existing directory")
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) if os.name == "nt" else 0
        self._waiting_for_device.clear()
        self._connected_to_device.clear()
        with self._server_output_lock:
            self._unknown_command_events = 0
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
                args=(self.server.stdout,),
                name="usbhostfs-output",
                daemon=True,
            )
            self._server_output_thread.start()
        if self.server.poll() is not None:
            raise RuntimeError("usbhostfs_pc exited during startup")

    def _observe_server_output(self, line: str) -> None:
        normalized = line.casefold()
        if "waiting for device" in normalized:
            self._waiting_for_device.set()
        if "connected to device" in normalized:
            self._connected_to_device.set()
        if "error, unknown command" in normalized:
            with self._server_output_lock:
                self._unknown_command_events += 1

    def _read_server_output(self, stream) -> None:
        try:
            for line in stream:
                self._observe_server_output(line)
        except (OSError, ValueError):
            return

    def take_waiting_for_device(self) -> bool:
        waiting = self._waiting_for_device.is_set()
        if waiting:
            self._waiting_for_device.clear()
        return waiting

    def take_unknown_command_events(self) -> int:
        with self._server_output_lock:
            events, self._unknown_command_events = self._unknown_command_events, 0
        return events

    @staticmethod
    def _command_succeeded(result: tuple[int | None, str, str, str]) -> bool:
        return result[0] == 0 and result[3] == "PROCESS_EXITED"

    def recover_psplink_transport(
        self,
        timeout: float,
        *,
        shell_verification_timeout: float = DEFAULT_SHELL_VERIFICATION_TIMEOUT,
        record_event: Callable[[str], None] | None = None,
    ) -> tuple[bool, str, tuple[int | None, str, str, str] | None]:
        """Reattach once, then qualify the fresh shell with bounded ``ver`` attempts."""

        self.take_waiting_for_device()
        list_result = self.command_runner(self.usbipd_argv + ["list"], timeout)
        if not self._command_succeeded(list_result):
            return (
                False,
                "usbipd list could not report the PSPLink device; manual command: `usbipd list`",
                None,
            )
        devices = _parse_usbipd_psplink_devices(list_result[1] + "\n" + list_result[2])
        if not devices:
            return (
                False,
                "PSPLink device 054c:01c9 is absent from usbipd list; reconnect the PSP, "
                "then run `usbipd list` and `usbipd attach --wsl --busid <BUSID>`",
                None,
            )
        if len(devices) != 1:
            return (
                False,
                "multiple PSPLink devices 054c:01c9 are present in usbipd list; "
                "disconnect extras and run `usbipd list` to identify the PSP busid",
                None,
            )

        busid, state = devices[0]
        if state.casefold() == "not shared":
            return (
                False,
                f"PSPLink device {busid} is not bound (usbipd state: Not shared); "
                f"manual commands: `usbipd bind --busid {busid}` then "
                f"`usbipd attach --wsl --busid {busid}`",
                None,
            )
        if state.casefold() not in {"shared", "attached"}:
            return (
                False,
                f"PSPLink device {busid} has unsupported usbipd state `{state}`; "
                f"manual command: `usbipd list`",
                None,
            )

        action = "already attached"
        if state.casefold() == "shared":
            action = "attached from Shared"
            self._connected_to_device.clear()
            attach_result = self.command_runner(
                self.usbipd_argv + ["attach", "--wsl", "--busid", busid], timeout
            )
            if not self._command_succeeded(attach_result):
                return (
                    False,
                    f"usbipd attach failed for PSPLink device {busid}; "
                    f"manual command: `usbipd attach --wsl --busid {busid}`",
                    None,
                )
            if not self._connected_to_device.wait(timeout):
                return (
                    False,
                    f"USBHostFS did not report `Connected to device` after attaching PSPLink "
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
        self, trigger: str, timeout: float
    ) -> tuple[bool, str, tuple[int | None, str, str, str] | None]:
        recover = getattr(self.transport, "recover_psplink_transport", None)
        if not callable(recover):
            detail = "transport adapter does not implement PSPLink re-attach"
            self._physical_intervention(f"{trigger}: {detail}")
            return False, detail, None
        self.recovery_events.append(f"L1: re-attach PSPLink transport after {trigger}")
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
            self._physical_intervention(f"{trigger}: {detail}")
            return False, detail, verification
        self.recovery_events.append(f"L1: {detail}")
        return True, detail, verification

    def _shell_qualified(self) -> bool:
        verified, _version, _attempts, detail = _verify_psplink_shell(
            self.transport.run,
            self.shell_verification_timeout,
            take_unknown_command_events=getattr(
                self.transport, "take_unknown_command_events", None
            ),
            record_event=self.recovery_events.append,
        )
        if not verified:
            self._physical_intervention(detail)
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

    def _unload(self, module_uid: str) -> bool:
        stopped = self._request(f"modstun {module_uid}", self.cleanup_timeout)
        if (
            not self._ok(stopped)
            or "Module Stop/Unload 0x00000000/" not in stopped[1]
        ):
            return False
        info = self._request(f"modinfo {module_uid}", self.cleanup_timeout)
        text = info[1] + info[2]
        return info[3] == "PROCESS_EXITED" and info[0] != 0 and "Unknown module" in text

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

    def _reset_once(self, detail: str) -> bool:
        if not self._shell_qualified():
            if self.terminal_reason is None:
                self._physical_intervention("PSPLink did not qualify before L2 reset")
            return False
        self.recovery_events.append(f"L2: {detail}")
        reset = self._request("reset", self.cleanup_timeout)
        reattached, _, _ = self._recover_usb_transport(
            "after PSPLink reset", self.cleanup_timeout
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
        if not self._qualify():
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
        self.recovery_events.append(f"L0: one shell qualification retry ({detail})")
        if self._shell_qualified():
            if module_uid and self._unload(module_uid):
                self.state = "READY"
                return True
        if self.terminal_reason is not None:
            return False

        self.recovery_events.append("L1: restart owned usbhostfs_pc process and requalify")
        try:
            self.transport.restart()
        except (OSError, RuntimeError):
            self._physical_intervention("host stack restart failed")
            return False
        if not self._qualify():
            if self.terminal_reason == "IDENTITY_MISMATCH":
                return False
            self._physical_intervention("PSPLink shell did not qualify after L1 restart")
            return False
        if module_uid and self._unload(module_uid):
            self.state = "READY"
            return True
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
    ) -> dict[str, object]:
        record_lines = []
        if result[3] == "PROCESS_EXITED":
            record_lines = [
                line for line in result[1].splitlines()
                if line.startswith(self._RECORD_PREFIXES)
            ]
        raw_result = "\n".join(record_lines) + ("\n" if record_lines else "")
        blockers: list[str] = []
        if result[3] == "TIMEOUT":
            blockers.append("per-case timeout; partial output is not a semantic result")
        elif result[0] != 0:
            blockers.append("PSPLink command did not exit successfully")
        if not record_lines:
            blockers.append("no complete source-owned scalar record was captured")
        if not module_uid or not cleanup_ok:
            blockers.append("loaded module was not proven unloaded")
        if not self.host0_qualified:
            blockers.append("host0 round-trip qualification did not pass")
        if self.terminal_reason:
            blockers.append(self.terminal_reason)
        binary_sha = self._sha256(case.binary)
        canonical = raw_result
        parsed_ok = False
        if raw_result:
            metadata_args = argparse.Namespace(
                binary=case.binary,
                model=self.console_model,
                model_code=self.model_code,
                firmware=self.firmware,
                source_commit=self.source_commit,
            )
            canonical = _canonicalize_psp(raw_result, metadata_args)
            try:
                parsed = parse_output(canonical)
                parsed_ok = bool(parsed.results) and all(
                    result.status == "PASS" for result in parsed.results
                )
                if not parsed_ok:
                    blockers.append("one or more scalar result records did not pass")
                metadata_problems = provenance_issues(parsed.metadata_dict())
                if metadata_problems:
                    blockers.append(
                        "captured metadata is not acceptance-eligible: "
                        + "; ".join(metadata_problems)
                    )
            except (ProtocolError, OSError, ValueError):
                blockers.append("captured scalar records failed strict protocol validation")
        if case.case_id == "transport-write" and not any(
            "test_id=PSP-TRANSPORT-001" in line
            and "case_id=host0-write-readback" in line
            and "status=PASS" in line
            for line in record_lines
        ):
            parsed_ok = False
            blockers.append("transport-write probe did not report a passing host0 round-trip")
        acceptance_eligible = parsed_ok and not blockers
        return {
            "CONSOLE_MODEL": self.console_model,
            "MODEL_SOURCE": "operator-recorded label; no serial or MAC stored",
            "SOFTWARE_MODEL_RAW_VALUE": str(self.model_code) if self.model_code is not None else "NOT_CAPTURED",
            "FW": self.firmware or "NOT_CAPTURED",
            "TRANSPORT_PROFILE": "standalone-psplink-usbhostfs-host0",
            "SOURCE_COMMIT": self.source_commit,
            "BINARY_SHA256": binary_sha,
            "CASE_ID": case.case_id,
            "RAW_RESULT": canonical,
            "RECOVERY_EVENTS": list(self.recovery_events),
            "QUALIFICATION_STATUS": "QUALIFIED" if self.state == "READY" else "LOST",
            "EVIDENCE_CLASS": "PSP_HARDWARE" if acceptance_eligible else "UNQUALIFIED_CAPTURE",
            "ACCEPTANCE_ELIGIBLE": acceptance_eligible,
            "ACCEPTANCE_BLOCKERS": blockers,
            "PROCESS_STATUS": result[3],
            "RETURN_CODE": result[0],
        }

    def run(self, cases: list[CampaignCase]) -> dict[str, object]:
        if not cases or cases[0].case_id != "transport-write":
            self.state = "STOPPED"
            self.terminal_reason = "HOST0_ROUNDTRIP_REQUIRED"
            return self._report()
        if any(not math.isfinite(case.timeout) or case.timeout <= 0 for case in cases):
            self.state = "STOPPED"
            self.terminal_reason = "INVALID_CASE_TIMEOUT"
            return self._report()
        host0_path = getattr(self.transport, "host0_root", None)
        if isinstance(host0_path, Path) and (
            host0_path / "nakagawa_transport_write.bin"
        ).exists():
            self.state = "STOPPED"
            self.terminal_reason = "HOST0_ROUNDTRIP_PATH_EXISTS"
            return self._report()
        try:
            self.transport.start()
        except (OSError, RuntimeError) as exc:
            self.transport.stop()
            self.state = "STOPPED"
            self.terminal_reason = f"HOST_TRANSPORT_NOT_READY: {type(exc).__name__}"
            return self._report()
        try:
            if not self._qualify():
                if self.terminal_reason is None:
                    self.state = "STOPPED"
                    self.terminal_reason = "SHELL_QUALIFICATION_FAILED"
                return self._report()
            for case in cases:
                self.state = "RUN_CASE"
                result = self._request(
                    f"ldstart host0:/{case.binary.name}", case.timeout
                )
                uid_match = self._MODULE_UID_RE.search(result[1])
                module_uid = uid_match.group(1) if uid_match else None
                cleanup_ok = bool(module_uid and self._unload(module_uid))
                if not cleanup_ok and self.terminal_reason is None:
                    self._recover(module_uid, f"cleanup after {case.case_id}")
                if case.case_id == "transport-write":
                    self.host0_qualified = self._verify_host0_roundtrip()
                    if not self.host0_qualified:
                        self.state = "STOPPED"
                        self.terminal_reason = "HOST0_ROUNDTRIP_FAILED"
                self.envelopes.append(
                    self._envelope(case, result, module_uid, cleanup_ok)
                )
                if self.terminal_reason:
                    break
                self.state = "READY"
        finally:
            self.transport.stop()
        return self._report()

    def _report(self) -> dict[str, object]:
        return {
            "schema": 1,
            "mode": "campaign",
            "state": self.state,
            "terminal_reason": self.terminal_reason,
            "firmware": self.firmware,
            "recovery_events": list(self.recovery_events),
            "envelopes": list(self.envelopes),
        }


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


def _wait_for_host0_output(
    path: Path,
    timeout: float,
    *,
    not_before_ns: int | None = None,
    ready: Callable[[str], bool],
) -> str:
    """Read a probe-owned host0 file after its complete stream is available."""

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            stat = path.stat()
            if (
                path.is_file()
                and stat.st_size > 0
                and (not_before_ns is None or stat.st_mtime_ns >= not_before_ns)
            ):
                first = path.read_bytes()
                time.sleep(0.05)
                second = path.read_bytes()
                if first == second:
                    stable = first.decode("utf-8", errors="replace")
                    if ready(stable):
                        return stable
        except OSError:
            pass
        time.sleep(0.1)
    raise TimeoutError(f"host0 output did not become complete: {path.name}")


def _host0_capture_complete(text: str, args: argparse.Namespace) -> bool:
    try:
        return len(parse_output(_canonicalize_psp(text, args)).results) == 16
    except (ProtocolError, OSError, ValueError):
        return False


def _validate_host0_capture(text: str, args: argparse.Namespace) -> dict[str, object]:
    """Validate the complete DMAC host0 stream without promoting placeholders."""

    parsed = validate_dmac_size_matrix(_canonicalize_psp(text, args))
    metadata = parsed.metadata_dict()
    blockers = list(provenance_issues(metadata))
    return {
        "classification": "PASS" if all(result.status == "PASS" for result in parsed.results) else "FAIL",
        "test_record_count": len(parsed.results),
        "metadata": metadata,
        "acceptance_eligible": not blockers,
        "acceptance_blockers": blockers,
    }


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
    model_fields = f"model={args.model}"
    if args.model_code is not None:
        generation, _retail = decode_psp_model_code(args.model_code)
        model_fields += f" model_code=0x{args.model_code:02x} model_generation={generation}"
    metadata = (
        "NAKAGAWA_PSP_META schema=1 source=psp "
        f"{model_fields} firmware={args.firmware} "
        f"binary_sha256={digest.hexdigest()} source_commit={args.source_commit}"
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
    parser.add_argument("--binary", type=Path, help="source-owned PRX used to replace fixture metadata")
    parser.add_argument("--source-commit", help="exact source commit recorded in the result metadata")
    parser.add_argument("--model", help="human-recorded PSP model identifier")
    parser.add_argument(
        "--model-code",
        type=lambda value: int(value, 0),
        help=(
            "PSPSDK/kubridge PspModel ordinal; derives the generation and retail family "
            "(do not use for sceKernelGetModel's original/slim return)"
        ),
    )
    parser.add_argument("--firmware", help="human-recorded PSP firmware identifier")
    parser.add_argument(
        "--campaign-case",
        action="append",
        default=[],
        metavar="CASE_ID=PRX_PATH",
        help="run an existing source-owned PRX from the host0 root; may be repeated",
    )
    parser.add_argument("--host0-root", type=Path, help="scratch directory shared by usbhostfs_pc")
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
    args.results_directory = args.results_directory.resolve()
    try:
        args.results_directory.relative_to(ROOT.resolve())
    except ValueError:
        parser.error("--results-directory must be inside the repository root")

    model_was_derived = args.model_code is not None
    if model_was_derived:
        if args.model:
            parser.error("--model and --model-code are mutually exclusive")
        try:
            generation, retail = decode_psp_model_code(args.model_code)
        except ValueError as exc:
            parser.error(str(exc))
        args.model = f"{retail}-{generation}"

    if args.campaign_case:
        if (
            args.command or args.annotate_report or args.psp_output or args.nakagawa_output
            or args.host0_output or args.dry_run or args.validate_dmac_size_matrix
        ):
            parser.error("campaign mode cannot be combined with single-capture or annotation options")
        if not args.host0_root or not args.model or not args.source_commit:
            parser.error("campaign mode requires --host0-root, --model/--model-code, and --source-commit")
        if not re.fullmatch(r"[0-9a-fA-F]{40}", args.source_commit):
            parser.error("--source-commit must be a full 40-digit commit id")
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
        if getattr(args, flag) and not (flag == "model" and model_was_derived)
    ]
    required_provenance_count = len(PROVENANCE_FLAGS) - (1 if model_was_derived else 0)
    if supplied and len(supplied) != required_provenance_count:
        missing_flags = [
            flag for flag in PROVENANCE_FLAGS
            if flag not in supplied and not (flag == "model" and model_was_derived)
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
