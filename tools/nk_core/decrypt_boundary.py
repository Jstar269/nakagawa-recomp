# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Run-time key-file location and boundary invocation (issue #295).

The built-in decryption boundary never carries key material: the user's
local key file lives under the private user-data root (or the override
named by ``NAKAGAWA_PSP_KEY_FILE``) and is never written into the
repository, next to an ISO, or into a published package.  Decrypted
bytes are staged only inside the private user-data cache and moved into
the per-title decrypted-module folder on success.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
import shutil
import subprocess
import tempfile

KEY_FILE_ENV = "NAKAGAWA_PSP_KEY_FILE"
BOUNDARY_TIMEOUT_SECONDS = 120

_REPO_ROOT = Path(__file__).resolve().parents[2]


def key_file_path(user_data_root: Path | str) -> Path:
    """The canonical local-only key file: env override, else <user data>/keys/psp-keyfile.json."""
    override = os.environ.get(KEY_FILE_ENV)
    if override:
        return Path(override).expanduser()
    return Path(user_data_root).expanduser() / "keys" / "psp-keyfile.json"


@dataclass(frozen=True)
class BoundaryOutcome:
    """Result of one boundary run: ok, no key file, or a fail-closed failure."""

    status: str  # "ok" | "no-keyfile" | "failed"
    detail: str = ""  # boundary token/message, for diagnostics
    key_path: str = ""  # the key file location that was consulted


def _run_boundary(key_path: Path, source: Path, destination: Path) -> BoundaryOutcome:
    from .decrypt_tool import find_or_build_tool

    try:
        tool = find_or_build_tool(_REPO_ROOT)
    except (OSError, RuntimeError) as exc:
        return BoundaryOutcome("failed", f"boundary unavailable: {exc}", str(key_path))
    destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    out_tmp = destination.with_name(destination.name + ".boundary.tmp")
    out_tmp.unlink(missing_ok=True)
    try:
        proc = subprocess.run(
            [str(tool), "decrypt", "--key-file", str(key_path),
             "--in", str(source), "--out", str(out_tmp)],
            capture_output=True, text=True,
            timeout=BOUNDARY_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return BoundaryOutcome("failed", f"boundary run failed: {exc}", str(key_path))
    if proc.returncode != 0 or not out_tmp.is_file():
        out_tmp.unlink(missing_ok=True)
        combined = (proc.stdout or "") + (proc.stderr or "")
        detail = next(
            (line.strip() for line in combined.splitlines()
             if line.strip() and not line.startswith("note:")),
            f"boundary exited with status {proc.returncode}",
        )
        return BoundaryOutcome("failed", detail, str(key_path))
    if os.name != "nt":
        out_tmp.chmod(0o600)
    os.replace(out_tmp, destination)
    return BoundaryOutcome("ok", "", str(key_path))


def decrypt_bytes_to(
    data: bytes,
    destination: Path,
    *,
    user_data_root: Path | str,
    key_file: Path | None = None,
) -> BoundaryOutcome:
    """Run the production boundary on ``data``; write the result to ``destination`` on success.

    The key file must exist or the outcome is ``no-keyfile`` (nothing is
    written).  Staging happens inside the private user-data cache only.
    """
    key_path = Path(key_file) if key_file is not None else key_file_path(user_data_root)
    if not key_path.is_file():
        return BoundaryOutcome("no-keyfile", key_path=str(key_path))
    root = Path(user_data_root).expanduser()
    stage_root = root / "cache" / "decrypted"
    stage_root.mkdir(parents=True, exist_ok=True, mode=0o700)
    resolved_root = root.resolve(strict=False)
    stage_resolved = stage_root.resolve(strict=False)
    try:
        stage_resolved.relative_to(resolved_root)
    except ValueError:
        return BoundaryOutcome("failed", "staging escaped the user data directory",
                               str(key_path))
    stage = Path(tempfile.mkdtemp(prefix="stage-", dir=stage_resolved))
    try:
        source = stage / "input.bin"
        source.write_bytes(data)
        if os.name != "nt":
            source.chmod(0o600)
        return _run_boundary(key_path, source, destination)
    except OSError as exc:
        return BoundaryOutcome("failed", f"staging failed: {exc}", str(key_path))
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def decrypt_file_inplace(
    path: Path,
    *,
    user_data_root: Path | str,
    key_file: Path | None = None,
) -> BoundaryOutcome:
    """Decrypt one container file over itself (private user-data staging only)."""
    try:
        data = Path(path).read_bytes()
    except OSError as exc:
        return BoundaryOutcome("failed", f"could not read staged module: {exc}")
    return decrypt_bytes_to(data, Path(path), user_data_root=user_data_root,
                            key_file=key_file)
