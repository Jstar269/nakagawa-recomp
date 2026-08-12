#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Exercise the pinned Betterleaks policy with synthetic, non-secret canaries.

The script deliberately constructs secret-shaped values from hashes at runtime,
so no credential-shaped literal is committed.  It is an explicit security gate,
not part of the ordinary Python unit-test discovery: the caller must provide the
Betterleaks binary installed from the immutable pre-commit revision.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parent.parent
CONFIG = ROOT / ".betterleaks.toml"


def _material(seed: str, length: int) -> str:
    value = ""
    counter = 0
    while len(value) < length:
        value += hashlib.sha256(f"{seed}-{counter}".encode("utf-8")).hexdigest()
        counter += 1
    return value[:length]


def _scan(binary: str, mode: str, target: str, *, extra: tuple[str, ...] = ()) -> int:
    command = [
        binary,
        mode,
        target,
        "--config",
        str(CONFIG),
        "--redact",
        *extra,
    ]
    result = subprocess.run(
        command,
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode


def _require(label: str, actual: int, expected: int) -> None:
    if actual != expected:
        raise RuntimeError(f"{label}: expected exit {expected}, got {actual}")
    print(f"{label}: PASS")


def _build_canaries(directory: Path) -> list[tuple[str, str]]:
    classic = "gh" + "p_" + _material("classic-pat", 36)
    fine_grained = "github" + "_pat_" + _material("fine-grained-pat", 82)
    api_key = _material("generic-api-key", 48)
    aws_access = ("AK" + "IA" + _material("aws-access", 16)).upper()
    aws_secret = _material("aws-secret", 40)
    bearer = _material("bearer-token", 48)
    generic = _material("generic-credential", 64)
    private_header = "-----BEGIN " + "RSA " + "PRIVATE KEY-----"
    private_footer = "-----END " + "RSA " + "PRIVATE KEY-----"
    return [
        ("classic-github-pat", f'token = "{classic}"\n'),
        ("fine-grained-github-pat", f'token = "{fine_grained}"\n'),
        ("generic-api-key", f'api_key = "{api_key}"\n'),
        (
            "aws-access-secret",
            f'aws_access_key_id = "{aws_access}"\naws_secret_access_key = "{aws_secret}"\n',
        ),
        ("bearer-token-assignment", f'bearer_token = "{bearer}"\n'),
        ("high-entropy-credential", f'credential = "{generic}"\n'),
        (
            "synthetic-private-key",
            f"{private_header}\n{_material('private-key', 64)}\n{private_footer}\n",
        ),
    ]


def run(binary: str) -> int:
    if not CONFIG.is_file():
        raise RuntimeError(f"missing Betterleaks config: {CONFIG}")
    with tempfile.TemporaryDirectory(prefix="nakagawa-betterleaks-") as raw:
        temp_root = Path(raw)

        for label, content in _build_canaries(temp_root):
            case = temp_root / f"{label}.txt"

            # This is a disposable, hash-derived synthetic payload whose only
            # purpose is to exercise the scanner; it is never a credential.
            # codeql[py/clear-text-storage-sensitive-data]
            case.write_text(content, encoding="utf-8")
            _require(label, _scan(binary, "dir", str(case)), 1)

        # The scanner must inspect encoded and archive-contained content when
        # the caller explicitly enables the documented traversal depths.
        encoded = temp_root / "encoded.txt"
        encoded.write_text(
            base64.b64encode(f'api_key = "{_material("encoded", 48)}"\n'.encode()).decode(),
            encoding="ascii",
        )
        _require(
            "base64-decoded-secret",
            _scan(binary, "dir", str(encoded), extra=("--max-decode-depth", "5")),
            1,
        )

        archive = temp_root / "archive.zip"
        with zipfile.ZipFile(archive, "w") as bundle:
            bundle.writestr("nested/secret.txt", f'api_key = "{_material("archive", 48)}"\n')
        _require(
            "archive-contained-secret",
            _scan(binary, "dir", str(archive), extra=("--max-archive-depth", "2")),
            1,
        )

        # The checked-in fixture is intentionally synthetic and must be
        # filtered; the filter is value- and path-specific, not tools/*.
        _require(
            "deliberate-history-audit-fixture",
            _scan(binary, "dir", "tools/test_history_audit.py"),
            0,
        )

        markdown = temp_root / "README.md"
        markdown.write_text(
            f"# Synthetic canary\n\napi_key = {_material('markdown', 48)}\n",
            encoding="utf-8",
        )
        _require("markdown-secret", _scan(binary, "dir", str(markdown)), 1)

        unusual_path = temp_root / "path-without-a-source-extension"
        unusual_path.write_text(
            f"credential = {_material('unusual-path', 64)}\n",
            encoding="utf-8",
        )
        _require("unusual-filename", _scan(binary, "dir", str(unusual_path)), 1)

        unsafe_tools = temp_root / "tools"
        unsafe_tools.mkdir()
        (unsafe_tools / "unsafe.py").write_text(
            f'api_key = "{_material("tools-unsafe", 48)}"\n', encoding="utf-8"
        )
        _require("tools-path-variation", _scan(binary, "dir", str(unsafe_tools)), 1)

        mixed_case_dir = temp_root / "mixed-case"
        mixed_case_dir.mkdir()
        mixed_case_path = mixed_case_dir / "ToKeN.TXT"
        mixed_case_path.write_text(
            f'api_key = "{_material("mixed-case-path", 48)}"\n', encoding="utf-8"
        )
        _require("mixed-case-path-variation", _scan(binary, "dir", str(mixed_case_path)), 1)

        moved_fixture = temp_root / "fixtures" / "history-audit-copy.py"
        moved_fixture.parent.mkdir()
        moved_fixture.write_text(
            f'api_key = "{_material("moved-fixture", 48)}"\n', encoding="utf-8"
        )
        _require("moved-outside-fixture-path", _scan(binary, "dir", str(moved_fixture)), 1)

        # A full reachable-history run protects the primary scanner from a
        # configuration change that only covers the current filesystem tree.
        _require("sanitized-history", _scan(binary, "git", "."), 0)

        # Git mode intentionally covers committed history, not ignored or
        # untracked working-tree bytes.  The directory mode remains the
        # publication-critical check for that local state; record both halves
        # so a future CLI/config change cannot silently alter the contract.
        ignored_repo = temp_root / "ignored-repo"
        ignored_repo.mkdir()
        for args in (
            ("init", "-q"),
            ("config", "user.name", "Betterleaks Canary"),
            ("config", "user.email", "betterleaks-canary@example.invalid"),
        ):
            subprocess.run(["git", *args], cwd=ignored_repo, check=True, capture_output=True)
        (ignored_repo / ".gitignore").write_text("ignored.txt\n", encoding="utf-8")
        subprocess.run(["git", "add", ".gitignore"], cwd=ignored_repo, check=True, capture_output=True)
        subprocess.run(["git", "commit", "-qm", "ignore synthetic canary"], cwd=ignored_repo, check=True)
        ignored = ignored_repo / "ignored.txt"
        ignored.write_text(
            f'api_key = "{_material("ignored-untracked", 48)}"\n', encoding="utf-8"
        )
        _require("ignored-untracked-directory", _scan(binary, "dir", str(ignored)), 1)
        _require("ignored-untracked-history", _scan(binary, "git", str(ignored_repo)), 0)

        # Prove the history path is not merely a current-tree check: put a
        # positive synthetic value in an ancestor commit and remove it before
        # scanning the resulting repository.
        ancestor = temp_root / "ancestor-repo"
        ancestor.mkdir()
        for args in (
            ("init", "-q"),
            ("config", "user.name", "Betterleaks Canary"),
            ("config", "user.email", "betterleaks-canary@example.invalid"),
        ):
            subprocess.run(["git", *args], cwd=ancestor, check=True, capture_output=True)
        historical = ancestor / "historical.txt"
        historical.write_text(f'api_key = "{_material("ancestor", 48)}"\n', encoding="utf-8")
        subprocess.run(["git", "add", "historical.txt"], cwd=ancestor, check=True, capture_output=True)
        subprocess.run(["git", "commit", "-qm", "synthetic canary"], cwd=ancestor, check=True)
        historical.unlink()
        subprocess.run(["git", "commit", "-qam", "remove synthetic canary"], cwd=ancestor, check=True)
        _require("ancestor-only-secret", _scan(binary, "git", str(ancestor)), 1)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--betterleaks",
        default=os.environ.get("BETTERLEAKS_BIN", "betterleaks"),
        help="Betterleaks executable (default: BETTERLEAKS_BIN or PATH)",
    )
    args = parser.parse_args(argv)
    try:
        return run(args.betterleaks)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"betterleaks canary: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
