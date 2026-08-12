# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp contributors

"""Check the explicit notices recorded in the inherited-file manifest.

This is a manifest-backed provenance gate, not a derivation classifier.  It
checks the exact notice strings selected by the audit and leaves legal
independence/derivation decisions to the documented human review.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any


MANIFEST_DEFAULT = Path("docs/provenance/MODIFIED_FILE_NOTICES.json")
GPL2_MARKERS = (
    "GNU GENERAL PUBLIC LICENSE",
    "Version 2, June 1991",
    "This License applies to any program or other work",
)
GPL3_MARKERS = ("GNU GENERAL PUBLIC LICENSE", "Version 3")


def _read_worktree(root: Path, relative: str) -> bytes | None:
    path = root / relative
    try:
        return path.read_bytes()
    except OSError:
        return None


def _read_index(root: Path, relative: str) -> bytes | None:
    proc = subprocess.run(
        ["git", "show", f":{relative}"],
        cwd=root,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    return proc.stdout if proc.returncode == 0 else None


def _read(root: Path, relative: str, tracked_only: bool) -> bytes | None:
    return _read_index(root, relative) if tracked_only else _read_worktree(root, relative)


def _text(data: bytes | None) -> str:
    return data.decode("utf-8", errors="replace") if data is not None else ""


def _add(findings: list[str], message: str) -> None:
    findings.append(message)


def audit(root: Path, manifest_relative: Path, tracked_only: bool) -> tuple[list[str], dict[str, int]]:
    findings: list[str] = []
    manifest_data = _read(root, manifest_relative.as_posix(), tracked_only)
    if manifest_data is None:
        return [f"missing manifest: {manifest_relative.as_posix()}"], {}
    try:
        manifest: dict[str, Any] = json.loads(_text(manifest_data))
    except json.JSONDecodeError as exc:
        return [f"manifest is not valid JSON: {exc}"], {}

    required = manifest.get("required_license_files")
    if not isinstance(required, list):
        _add(findings, "manifest required_license_files must be a list")
        required = []
    for entry in required:
        if not isinstance(entry, dict) or not isinstance(entry.get("path"), str):
            _add(findings, "manifest contains an invalid required license-file entry")
            continue
        path = entry["path"]
        content = _read(root, path, tracked_only)
        if not content:
            _add(findings, f"required license/notice file missing or empty: {path}")
            continue
        text = _text(content)
        if path == "LICENSE" and not all(marker in text for marker in GPL3_MARKERS):
            _add(findings, "LICENSE does not contain the repository GPL-3.0 text markers")
        if path.endswith("THIRD_PARTY_LICENSES/GPL-2.0.txt") and not all(
            marker in text for marker in GPL2_MARKERS
        ):
            _add(findings, "THIRD_PARTY_LICENSES/GPL-2.0.txt does not contain GPLv2 text markers")

    records = manifest.get("files")
    if not isinstance(records, list):
        _add(findings, "manifest files must be a list")
        records = []
    seen: set[str] = set()
    checked_text = 0
    checked_generated = 0
    for record in records:
        if not isinstance(record, dict) or not isinstance(record.get("path"), str):
            _add(findings, "manifest contains a file entry without a path")
            continue
        path = record["path"]
        if path in seen:
            _add(findings, f"manifest contains a duplicate path: {path}")
        seen.add(path)
        if Path(path).is_absolute() or ".." in Path(path).parts:
            _add(findings, f"manifest path escapes repository scope: {path}")
            continue
        data = _read(root, path, tracked_only)
        kind = record.get("source_kind")
        if kind == "generated_output":
            checked_generated += 1
            source = record.get("generated_from")
            if data is None:
                _add(findings, f"generated output is missing: {path}")
            if not isinstance(source, str) or _read(root, source, tracked_only) is None:
                _add(findings, f"generated source is missing for {path}: {source}")
            continue
        checked_text += 1
        if data is None:
            _add(findings, f"manifest source file is missing: {path}")
            continue
        text = _text(data)
        spdx = record.get("spdx")
        if not isinstance(spdx, str) or f"SPDX-License-Identifier: {spdx}" not in text:
            _add(findings, f"{path}: expected SPDX identifier is absent")
        expected = record.get("expected_notice")
        if not isinstance(expected, dict) or expected.get("attribution_required") is not True:
            _add(findings, f"{path}: manifest does not require immediate-upstream attribution")
            continue
        if "PSP-recompilation-project" not in text and "sal063" not in text:
            _add(findings, f"{path}: immediate sal063 attribution is absent")
        if expected.get("modification_required") is True:
            date = expected.get("date")
            if not isinstance(date, str) or not re.fullmatch(r"\d{4}-\d{2}-\d{2}", date):
                _add(findings, f"{path}: invalid modification date in manifest")
            else:
                notice = f"Modified by Nakagawa Recomp contributors, {date}."
                if notice not in text:
                    _add(findings, f"{path}: explicit modified-file notice is absent")
            pointer = expected.get("pointer")
            if not isinstance(pointer, str) or pointer not in text:
                _add(findings, f"{path}: provenance pointer is absent")

    return findings, {"textual": checked_text, "generated": checked_generated}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=MANIFEST_DEFAULT)
    parser.add_argument("--tracked-only", action="store_true", help="read staged/index blobs")
    parser.add_argument("--worktree", action="store_true", help="read worktree bytes (overrides --tracked-only)")
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    tracked_only = args.tracked_only and not args.worktree
    findings, counts = audit(root, args.manifest, tracked_only)
    source = "index" if tracked_only else "worktree"
    print(f"modified-file notice audit source: {source}")
    print(f"modified-file notice audit files: textual={counts.get('textual', 0)} generated={counts.get('generated', 0)}")
    if findings:
        for finding in findings:
            print(f"FAIL: {finding}")
        print("MODIFIED_FILE_NOTICE_AUDIT: FAIL")
        return 1
    print("MODIFIED_FILE_NOTICE_AUDIT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
