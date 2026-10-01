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
from typing import Callable

_PACKAGE_PARENT = str(Path(__file__).resolve().parents[1])
if _PACKAGE_PARENT not in sys.path:
    sys.path.insert(0, _PACKAGE_PARENT)

try:
    from .protocol import (
        ProtocolError,
        compare_texts,
        decode_psp_model_code,
        dump_json,
        ge_corpus_report,
        parse_output,
        provenance_issues,
        validate_dmac_size_matrix,
        validate_dmac_size_matrix_size,
    )
except ImportError:  # direct ``python tools/psp_oracle/run_psplink.py`` invocation
    from psp_oracle.protocol import (
        ProtocolError,
        compare_texts,
        decode_psp_model_code,
        dump_json,
        ge_corpus_report,
        parse_output,
        provenance_issues,
        validate_dmac_size_matrix,
        validate_dmac_size_matrix_size,
    )

try:
    from .parse_golden import (
        parse_cache_alias_output,
        parse_fpu_vector_output,
        parse_io_matrix_output,
        parse_mbx_delete_wait_output,
    )
except ImportError:  # direct ``python tools/psp_oracle/run_psplink.py`` invocation
    from psp_oracle.parse_golden import (
        parse_cache_alias_output,
        parse_fpu_vector_output,
        parse_io_matrix_output,
        parse_mbx_delete_wait_output,
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
_FULL_SHA256_RE = re.compile(r"[0-9a-fA-F]{64}")
_ALL_ZERO_RE = re.compile(r"0+")


class UnsafeHost0OutputError(OSError):
    """A host0 result path is not a regular file owned by the scratch root."""


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


def _parse_campaign_records(text: str, case_id: str):
    """Validate a campaign's known completion contract, then parse its rows."""

    if case_id in {"dma-size-matrix", "dmac-size-matrix"}:
        return validate_dmac_size_matrix(text)
    match = re.fullmatch(r"dmac-size-matrix-size-0x([0-9a-f]{8})", case_id)
    if match:
        return validate_dmac_size_matrix_size(text, int(match.group(1), 16))

    parsed = parse_output(text)
    complete_parser = {
        "fpu-vector": parse_fpu_vector_output,
        "cache-alias": parse_cache_alias_output,
        "io-matrix": parse_io_matrix_output,
        "mbx-delete-wait": parse_mbx_delete_wait_output,
    }.get(case_id)
    if complete_parser is not None:
        complete_parser(text, require_complete=True)
    else:
        expected_single = {
            "transport-write": ("PSP-TRANSPORT-001", "host0-write-readback"),
            "model-profile": ("PSP-SYSTEM-001", "model-profile"),
        }.get(case_id)
        if expected_single is not None:
            if len(parsed.results) != 1:
                raise ProtocolError(
                    f"{case_id} stream must contain exactly one result record"
                )
            record = parsed.results[0]
            if (record.test_id, record.case_id) != expected_single:
                raise ProtocolError(
                    f"{case_id} stream must contain {expected_single[0]}/{expected_single[1]}"
                )
    return parsed


def _campaign_stream_complete(text: str, case_id: str) -> bool:
    """Return whether a fresh campaign stream satisfies its known completion contract."""

    if _campaign_completeness_contract(case_id) == "unregistered-no-completion-contract":
        return False
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
    if case_id in {"dma-size-matrix", "dmac-size-matrix"} or re.fullmatch(
        r"dmac-size-matrix-size-0x[0-9a-f]{8}", case_id
    ):
        return "strict-dmac-sequence"
    if case_id in {"fpu-vector", "cache-alias", "io-matrix", "mbx-delete-wait"}:
        return "strict-golden-sequence"
    if case_id in {"transport-write", "model-profile"}:
        return "exactly-one-known-record"
    return "unregistered-no-completion-contract"


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
        self.source_tree_problem: str | None = None

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
        *,
        host0_log_path: Path | None,
        run_started_ns: int | None,
        run_finished_ns: int,
        host0_log_cleared: bool,
        captured_host0_text: str | None,
        captured_host0_mtime_ns: int | None,
        host0_capture_problem: str | None,
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
        if result[3] == "TIMEOUT":
            disqualify("per-case timeout; partial output is not a semantic result")
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
                    _normalise_unbound_identity_fields(host0_text), case.case_id
                )
                canonical = _canonicalize_psp(host0_text, metadata_args)
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
            except (ProtocolError, OSError, UnicodeError, ValueError):
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
        return {
            "CONSOLE_MODEL": self.console_model,
            "MODEL_SOURCE": "operator-recorded label; no serial or MAC stored",
            "SOFTWARE_MODEL_RAW_VALUE": str(self.model_code) if self.model_code is not None else "NOT_CAPTURED",
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
                "QUALIFIED" if self.state in ("RUN_CASE", "READY") else "LOST"
            ),
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
                    break

                run_started_ns = time.time_ns()
                self.state = "RUN_CASE"
                result = self._request(
                    f"ldstart host0:/{case.binary.name}", case.timeout
                )
                uid_match = self._MODULE_UID_RE.search(result[1])
                module_uid = uid_match.group(1) if uid_match else None

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
                        capture = _wait_for_host0_output(
                            case_host0_log,
                            case.timeout,
                            not_before_ns=run_started_ns - HOST0_MTIME_TOLERANCE_NS,
                            ready=lambda text, case_id=case.case_id: _campaign_stream_complete(
                                text, case_id
                            ),
                            include_mtime=True,
                        )
                        if not isinstance(capture, tuple):
                            raise RuntimeError("host0 wait did not return captured metadata")
                        captured_host0_text, captured_host0_mtime_ns = capture
                    except TimeoutError:
                        host0_capture_problem = (
                            "per-case host0 stream did not become complete before probe unload"
                        )
                        partial, partial_mtime_ns, partial_problem = _snapshot_host0_output(
                            case_host0_log
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

                host0_roundtrip_ok: bool | None = None
                if case.case_id == "transport-write":
                    host0_roundtrip_ok = self._verify_host0_roundtrip()

                cleanup_ok = bool(module_uid and self._unload(module_uid))
                if not cleanup_ok and self.terminal_reason is None:
                    self._recover(module_uid, f"cleanup after {case.case_id}")
                run_finished_ns = time.time_ns()
                if case.case_id == "transport-write":
                    self.host0_qualified = bool(host0_roundtrip_ok)
                    if not self.host0_qualified:
                        self.state = "STOPPED"
                        self.terminal_reason = "HOST0_ROUNDTRIP_FAILED"
                self.envelopes.append(
                    self._envelope(
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
                    )
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
    """

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
    model_code = getattr(args, "model_code", None)
    if model_code is not None:
        generation, _retail = decode_psp_model_code(model_code)
        model_fields += f" model_code=0x{model_code:02x} model_generation={generation}"

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
    if args.ge_corpus_gate:
        if any((
            args.prx, args.remote_command, args.command, args.psp_output, args.nakagawa_output,
            args.host0_output, args.validate_dmac_size_matrix, args.binary, args.source_commit,
            args.model, args.model_code is not None, args.firmware, args.campaign_case,
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
        report = ge_corpus_report(corpus, schema)
        sys.stdout.write(dump_json(report))
        return 2 if report["status"] == "REFUSED" else 0
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
