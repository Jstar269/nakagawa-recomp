#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Produce bounded local PSPDEV tool identity evidence without changing the lock."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import threading
import time
from typing import Any, Callable

import pspdev_lock

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "build" / "audit" / "pspdev-tool-probe.json"
MAX_EXECUTABLE_BYTES = 512 * 1024 * 1024
MAX_OUTPUT_BYTES = 64 * 1024
PROBE_TIMEOUT_SECONDS = 5.0

TOOL_ARGS: dict[str, tuple[str, ...]] = {
    "psp-config": ("--pspsdk-path",),
    "psp-gcc": ("--version",),
    "psp-ld": ("--version",),
    "psp-objdump": ("--version",),
    "psp-readelf": ("--version",),
    "psp-nm": ("--version",),
    "psp-prxgen": ("--version",),
    "psp-build-exports": ("--help",),
    "psp-fixup-imports": ("--help",),
    "mksfoex": ("--help",),
    "pack-pbp": (),
    "pspsh": ("--help",),
    "usbhostfs_pc": ("--help",),
}


class ProbeError(RuntimeError):
    """A local executable could not be safely fingerprinted."""


ABSOLUTE_PATH_PATTERNS = (
    re.compile(r"[A-Za-z]:[\\/][^\s\r\n]+"),
    re.compile(r"\\\\[^\s\r\n]+"),
    re.compile(r"(?<![:A-Za-z0-9])/(?:[^\s\r\n]+)"),
)


def _sanitize_output(text: str, *, include_paths: bool, resolved: Path) -> str:
    if include_paths:
        return text
    sanitized = text.replace(str(resolved), "<redacted-path>")
    for pattern in ABSOLUTE_PATH_PATTERNS:
        sanitized = pattern.sub("<redacted-path>", sanitized)
    return sanitized


def _safe_error(exc: BaseException) -> str:
    return type(exc).__name__


def _run_bounded_process(
    command: list[str],
    *,
    timeout: float = PROBE_TIMEOUT_SECONDS,
    max_output: int = MAX_OUTPUT_BYTES,
) -> dict[str, Any]:
    """Run one command while bounding both captured streams during execution."""

    try:
        process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            shell=False,
            bufsize=0,
        )
    except OSError as exc:
        return {
            "status": "exec_error",
            "returncode": None,
            "stdout": b"",
            "stderr": b"",
            "stdout_truncated": False,
            "stderr_truncated": False,
            "error": _safe_error(exc),
        }

    buffers = [bytearray(), bytearray()]
    truncated = [False, False]
    output_limit = threading.Event()

    def drain(stream: Any, index: int) -> None:
        try:
            while True:
                chunk = stream.read(4096)
                if not chunk:
                    break
                remaining = max_output - len(buffers[index])
                if remaining > 0:
                    buffers[index].extend(chunk[:remaining])
                if len(chunk) > remaining:
                    truncated[index] = True
                    output_limit.set()
                    break
        except (OSError, ValueError):
            # Process shutdown may close a pipe while its reader is blocked.
            pass

    streams = [process.stdout, process.stderr]
    threads = [
        threading.Thread(target=drain, args=(stream, index), daemon=True)
        for index, stream in enumerate(streams)
    ]
    for thread in threads:
        thread.start()

    deadline = time.monotonic() + timeout
    status = "completed"
    while process.poll() is None:
        if output_limit.is_set():
            status = "output_limit"
            process.kill()
            break
        if time.monotonic() >= deadline:
            status = "timeout"
            process.kill()
            break
        time.sleep(0.01)

    try:
        process.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2.0)
    for stream in streams:
        if stream is not None:
            try:
                stream.close()
            except OSError:
                pass
    for thread in threads:
        thread.join(timeout=1.0)
    if output_limit.is_set() and status == "completed":
        status = "output_limit"

    return {
        "status": status,
        "returncode": process.returncode,
        "stdout": bytes(buffers[0]),
        "stderr": bytes(buffers[1]),
        "stdout_truncated": truncated[0],
        "stderr_truncated": truncated[1],
    }


def _sha256_file(path: Path, *, max_bytes: int = MAX_EXECUTABLE_BYTES) -> tuple[str, int]:
    try:
        size = path.stat().st_size
    except OSError as exc:
        raise ProbeError(f"cannot stat {path.name}: {exc}") from exc
    if size < 0 or size > max_bytes:
        raise ProbeError(f"{path.name} size {size} exceeds probe cap {max_bytes}")
    digest = hashlib.sha256()
    seen = 0
    try:
        with path.open("rb") as stream:
            while True:
                chunk = stream.read(1024 * 1024)
                if not chunk:
                    break
                seen += len(chunk)
                if seen > max_bytes:
                    raise ProbeError(f"{path.name} grew beyond probe cap while hashing")
                digest.update(chunk)
    except OSError as exc:
        raise ProbeError(f"cannot hash {path.name}: {exc}") from exc
    if seen != size:
        raise ProbeError(f"{path.name} changed size while hashing ({size} -> {seen})")
    return digest.hexdigest(), seen


def _bounded_text(data: bytes) -> tuple[str, bool]:
    truncated = len(data) > MAX_OUTPUT_BYTES
    clipped = data[:MAX_OUTPUT_BYTES]
    return clipped.decode("utf-8", errors="replace").replace("\r\n", "\n"), truncated


def probe_executable(
    name: str,
    path: Path,
    args: tuple[str, ...],
    *,
    include_paths: bool = False,
    runner: Callable[..., subprocess.CompletedProcess[bytes]] | None = None,
) -> dict[str, Any]:
    """Fingerprint one resolved executable and run one bounded identity command."""

    resolved = path.resolve(strict=True)
    if not resolved.is_file():
        raise ProbeError(f"{name} resolved target is not a regular file")
    binary_sha256, size = _sha256_file(resolved)
    command = [str(resolved), *args]
    if runner is None:
        execution = _run_bounded_process(command)
        status = execution["status"]
        if status == "exec_error":
            result = {
                "name": name,
                "status": "exec_error",
                "resolved_name": resolved.name,
                "path_sha256": hashlib.sha256(
                    str(resolved).encode("utf-8")
                ).hexdigest(),
                "binary_sha256": binary_sha256,
                "size": size,
                "command_args": list(args),
                "error": execution["error"],
            }
        elif status == "timeout":
            result = {
                "name": name,
                "status": "timeout",
                "resolved_name": resolved.name,
                "path_sha256": hashlib.sha256(
                    str(resolved).encode("utf-8")
                ).hexdigest(),
                "binary_sha256": binary_sha256,
                "size": size,
                "command_args": list(args),
                "timeout_seconds": PROBE_TIMEOUT_SECONDS,
                "stdout": _sanitize_output(
                    _bounded_text(execution["stdout"])[0],
                    include_paths=include_paths,
                    resolved=resolved,
                ),
                "stderr": _sanitize_output(
                    _bounded_text(execution["stderr"])[0],
                    include_paths=include_paths,
                    resolved=resolved,
                ),
                "stdout_truncated": execution["stdout_truncated"],
                "stderr_truncated": execution["stderr_truncated"],
            }
        else:
            stdout, _ = _bounded_text(execution["stdout"])
            stderr, _ = _bounded_text(execution["stderr"])
            stdout = _sanitize_output(
                stdout, include_paths=include_paths, resolved=resolved
            )
            stderr = _sanitize_output(
                stderr, include_paths=include_paths, resolved=resolved
            )
            result = {
                "name": name,
                "status": (
                    "output_limit"
                    if status == "output_limit"
                    else ("ok" if execution["returncode"] == 0 else "nonzero")
                ),
                "resolved_name": resolved.name,
                "path_sha256": hashlib.sha256(
                    str(resolved).encode("utf-8")
                ).hexdigest(),
                "binary_sha256": binary_sha256,
                "size": size,
                "command_args": list(args),
                "returncode": execution["returncode"],
                "stdout": stdout,
                "stderr": stderr,
                "stdout_truncated": execution["stdout_truncated"],
                "stderr_truncated": execution["stderr_truncated"],
            }
    else:
        try:
            completed = runner(
                command,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                shell=False,
                check=False,
                timeout=PROBE_TIMEOUT_SECONDS,
            )
        except subprocess.TimeoutExpired:
            result = {
                "name": name,
                "status": "timeout",
                "resolved_name": resolved.name,
                "path_sha256": hashlib.sha256(
                    str(resolved).encode("utf-8")
                ).hexdigest(),
                "binary_sha256": binary_sha256,
                "size": size,
                "command_args": list(args),
                "timeout_seconds": PROBE_TIMEOUT_SECONDS,
            }
        except OSError as exc:
            result = {
                "name": name,
                "status": "exec_error",
                "resolved_name": resolved.name,
                "path_sha256": hashlib.sha256(
                    str(resolved).encode("utf-8")
                ).hexdigest(),
                "binary_sha256": binary_sha256,
                "size": size,
                "command_args": list(args),
                "error": _safe_error(exc),
            }
        else:
            stdout, stdout_truncated = _bounded_text(completed.stdout or b"")
            stderr, stderr_truncated = _bounded_text(completed.stderr or b"")
            stdout = _sanitize_output(
                stdout, include_paths=include_paths, resolved=resolved
            )
            stderr = _sanitize_output(
                stderr, include_paths=include_paths, resolved=resolved
            )
            result = {
                "name": name,
                "status": "ok" if completed.returncode == 0 else "nonzero",
                "resolved_name": resolved.name,
                "path_sha256": hashlib.sha256(
                    str(resolved).encode("utf-8")
                ).hexdigest(),
                "binary_sha256": binary_sha256,
                "size": size,
                "command_args": list(args),
                "returncode": completed.returncode,
                "stdout": stdout,
                "stderr": stderr,
                "stdout_truncated": stdout_truncated,
                "stderr_truncated": stderr_truncated,
            }
    if include_paths:
        result["path"] = str(resolved)
    return result


def probe_tools(
    *,
    search_path: str | None = None,
    include_paths: bool = False,
    resolver: Callable[..., str | None] = shutil.which,
    runner: Callable[..., subprocess.CompletedProcess[bytes]] | None = None,
) -> dict[str, Any]:
    results = []
    for name in sorted(TOOL_ARGS):
        located = resolver(name, path=search_path)
        if not located:
            results.append({"name": name, "status": "missing"})
            continue
        try:
            results.append(
                probe_executable(
                    name,
                    Path(located),
                    TOOL_ARGS[name],
                    include_paths=include_paths,
                    runner=runner,
                )
            )
        except (OSError, ProbeError) as exc:
            results.append(
                {"name": name, "status": "probe_error", "error": _safe_error(exc)}
            )
    return {
        "schema": 1,
        "host": {
            "system": platform.system(),
            "machine": platform.machine(),
            "python": platform.python_version(),
        },
        "privacy": {
            "absolute_paths_included": include_paths,
            "default_path_representation": "basename plus SHA-256 of canonical path",
        },
        "limits": {
            "executable_bytes": MAX_EXECUTABLE_BYTES,
            "command_output_bytes_per_stream": MAX_OUTPUT_BYTES,
            "timeout_seconds": PROBE_TIMEOUT_SECONDS,
        },
        "tools": results,
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--lock", type=Path, default=pspdev_lock.DEFAULT_LOCK)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument(
        "--path",
        help="explicit executable search PATH; defaults to the current process PATH",
    )
    parser.add_argument(
        "--include-paths",
        action="store_true",
        help="include canonical local paths in the untracked report",
    )
    parser.add_argument("--require-all", action="store_true")
    args = parser.parse_args(argv)

    try:
        data, _ = pspdev_lock.load_lock(args.lock)
        pspdev_lock.validate_lock(data)
    except pspdev_lock.LockError as exc:
        print(f"pspdev_probe: invalid lock: {exc}", file=sys.stderr)
        return 1

    report = probe_tools(
        search_path=args.path if args.path is not None else os.environ.get("PATH"),
        include_paths=args.include_paths,
    )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(
        json.dumps(report, indent=2, sort_keys=True, ensure_ascii=True) + "\n",
        encoding="ascii",
        newline="\n",
    )
    missing = [
        item["name"] for item in report["tools"] if item["status"] == "missing"
    ]
    errors = [
        item["name"]
        for item in report["tools"]
        if item["status"]
        in {"timeout", "output_limit", "exec_error", "probe_error"}
    ]
    print(
        f"pspdev_probe: {len(report['tools']) - len(missing)} located, "
        f"{len(missing)} missing, {len(errors)} probe errors -> {args.out}"
    )
    if args.require_all and (missing or errors):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
