#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Classify a PSP ELF's imports against the HLE registration manifest.

Reads a developer-supplied ELF (never committed; see docs/IMPORT_AUDIT.md),
parses its import table defensively, and classifies every (library, NID)
import as one of:

    missing                -- no static HLE registration; under the scheduler a
                              call terminates the process (_Exit(7) in hle.c)
    fake_success           -- registered to a generic always-success handler;
                              silently fabricates success
    dedicated              -- has its own handler (status metadata says how
                              finished that handler is believed to be)
    controlled_unsupported -- refuses with the API's documented error code

Reports are deterministic (sorted, ASCII, LF, no timestamps) so private runs
can be diffed across machines and revisions without committing them. Output
is API/library-level by default: guest stub addresses are only emitted with
--with-addresses, so a redacted report contains nothing title-specific beyond
which public API names the title imports.

This tool never fails on coverage (a private title importing unsupported
functions is expected); it exits nonzero only for unreadable or malformed
input. Public CI policy lives in tools/import_audit_gate.py.

Usage:
    python tools/hle_manifest.py --out build/hle_manifest.json
    python tools/import_audit.py --elf <path> --manifest build/hle_manifest.json \
        --out build/private/import_audit.json --text build/private/import_audit.txt
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hle_registry_meta as meta  # noqa: E402
import psp_import_table  # noqa: E402
from psp_import_table import ImportTableError, parse_import_table  # noqa: E402

REPORT_SCHEMA = 1
# Must match hle_manifest.MANIFEST_SCHEMA (asserted by tests) without pulling
# the extractor in at import time.
EXPECTED_MANIFEST_SCHEMA = 1
CLASSES = ("missing", "fake_success", "dedicated", "controlled_unsupported")
REGISTERED_CLASSES = frozenset(c for c in CLASSES if c != "missing")
_NID_RE = re.compile(r"^0x[0-9a-f]{8}$")
_MANIFEST_ROW_FIELDS = ("nid", "name", "handler", "origin", "classification", "status")


class ManifestValidationError(Exception):
    """A --manifest file that cannot be trusted. Message is one line."""


def _read_bounded(path: Path, cap: int) -> bytes | None:
    """Read at most cap bytes (+1 sentinel); None if the file exceeds cap.

    The pre-read stat() in main() is only a fast path -- a file that grows
    between stat and read must still never allocate beyond the cap, so the
    read itself is bounded instead of trusting the stat result.
    """
    with open(path, "rb") as f:
        data = f.read(cap + 1)
    return None if len(data) > cap else data


def load_manifest(path: Path) -> dict:
    """Read and validate an hle_manifest.json produced by tools/hle_manifest.py."""
    try:
        text = path.read_text(encoding="ascii")
    except OSError as e:
        raise ManifestValidationError(f"cannot read {path}: {e.strerror}") from e
    except UnicodeDecodeError as e:
        raise ManifestValidationError(f"{path} is not ASCII JSON: {e.reason}") from e
    try:
        manifest = json.loads(text)
    except json.JSONDecodeError as e:
        raise ManifestValidationError(f"{path} is not valid JSON: {e.msg} at line {e.lineno}") from e
    if not isinstance(manifest, dict):
        raise ManifestValidationError(f"{path}: top level must be an object")
    if manifest.get("schema") != EXPECTED_MANIFEST_SCHEMA:
        raise ManifestValidationError(
            f"{path}: schema {manifest.get('schema')!r} is not {EXPECTED_MANIFEST_SCHEMA} "
            "(regenerate with tools/hle_manifest.py)"
        )
    regs = manifest.get("registrations")
    if not isinstance(regs, list) or not regs:
        raise ManifestValidationError(f"{path}: 'registrations' must be a non-empty list")
    seen: set[str] = set()
    for i, row in enumerate(regs):
        if not isinstance(row, dict):
            raise ManifestValidationError(f"{path}: registrations[{i}] is not an object")
        for fieldname in _MANIFEST_ROW_FIELDS:
            if not isinstance(row.get(fieldname), str) or not row[fieldname]:
                raise ManifestValidationError(
                    f"{path}: registrations[{i}] missing or non-string field {fieldname!r}"
                )
        if not _NID_RE.match(row["nid"]):
            raise ManifestValidationError(
                f"{path}: registrations[{i}] invalid NID {row['nid']!r} (want 0x + 8 lowercase hex)"
            )
        if row["classification"] not in REGISTERED_CLASSES:
            raise ManifestValidationError(
                f"{path}: registrations[{i}] invalid classification {row['classification']!r}"
            )
        if row["status"] not in meta.HANDLER_STATUSES:
            raise ManifestValidationError(
                f"{path}: registrations[{i}] invalid status {row['status']!r}"
            )
        if row["nid"] in seen:
            raise ManifestValidationError(f"{path}: duplicate NID {row['nid']} in registrations")
        seen.add(row["nid"])
    return manifest


def classify_imports(
    table_funcs, manifest: dict, *, with_addresses: bool = False, findings=()
) -> dict:
    """Build the deterministic report dict for parsed imports against a manifest.

    findings carries structural notes from the import-table parser (uncovered
    or multi-claimed stub slots in interleaved tables); the parser emits them
    in deterministic order and they pass through untouched.
    """
    regs = {int(r["nid"], 16): r for r in manifest["registrations"]}

    rows = []
    by_nid_libs: dict[int, set[str]] = {}
    for f in table_funcs:
        by_nid_libs.setdefault(f.nid, set()).add(f.library)
        reg = regs.get(f.nid)
        row = {
            "lib": f.library,
            "nid": f"0x{f.nid:08x}",
            "name": reg["name"] if reg else meta.KNOWN_NID_NAMES.get(f.nid),
            "classification": reg["classification"] if reg else "missing",
            "handler": reg["handler"] if reg else None,
            "status": reg["status"] if reg else None,
        }
        if with_addresses:
            row["stub"] = f"0x{f.stub_addr:08x}"
        rows.append(row)
    rows.sort(key=lambda r: (r["lib"], r["nid"]))

    lib_stats: dict[str, dict] = {}
    totals = {c: 0 for c in CLASSES}
    for r in rows:
        totals[r["classification"]] += 1
        s = lib_stats.setdefault(r["lib"], {c: 0 for c in CLASSES})
        s[r["classification"]] += 1

    duplicates = [
        {"nid": f"0x{nid:08x}", "libs": sorted(libs)}
        for nid, libs in sorted(by_nid_libs.items())
        if len(libs) > 1
    ]

    return {
        "schema": REPORT_SCHEMA,
        "summary": {"total": len(rows), **totals, "libraries": len(lib_stats)},
        "libraries": [
            {"name": name, "total": sum(s.values()), **s} for name, s in sorted(lib_stats.items())
        ],
        "imports": rows,
        "cross_library_duplicate_nids": duplicates,
        "findings": list(findings),
    }


def render_text(report: dict) -> str:
    """Human-readable report with a triage section for actionable imports."""
    out = []
    s = report["summary"]
    out.append("PSP import-coverage audit")
    out.append(
        f"  imports: {s['total']} across {s['libraries']} libraries | "
        f"dedicated {s['dedicated']}, controlled_unsupported {s['controlled_unsupported']}, "
        f"fake_success {s['fake_success']}, missing {s['missing']}"
    )
    out.append("")
    for lib in report["libraries"]:
        out.append(
            f"  {lib['name']}: total {lib['total']}, dedicated {lib['dedicated']}, "
            f"controlled_unsupported {lib['controlled_unsupported']}, "
            f"fake_success {lib['fake_success']}, missing {lib['missing']}"
        )
    dupes = report["cross_library_duplicate_nids"]
    if dupes:
        out.append("")
        out.append("  duplicate NIDs imported by multiple libraries:")
        for d in dupes:
            out.append(f"    {d['nid']}: {', '.join(d['libs'])}")

    if report["findings"]:
        out.append("")
        out.append("  structural findings (interleaved stub table):")
        for finding in report["findings"]:
            out.append(f"    {finding}")

    triage = [r for r in report["imports"] if r["classification"] in ("missing", "fake_success")]
    out.append("")
    if triage:
        out.append("Triage: imports that terminate the scheduler (missing) or silently")
        out.append("fabricate success (fake_success). File one focused implementation issue")
        out.append("per API family instead of broadly mapping NIDs to h_ok:")
        for r in triage:
            label = r["name"] or "(unknown name)"
            out.append(f"  [{r['classification']}] {r['lib']} {r['nid']} {label}")
    else:
        out.append("Triage: no missing or fake-success imports.")
    out.append("")
    return "\n".join(out)


def _dump(obj_or_text, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="ascii", newline="\n") as f:
        if isinstance(obj_or_text, str):
            f.write(obj_or_text)
        else:
            json.dump(obj_or_text, f, indent=2, sort_keys=True)
            f.write("\n")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--elf", required=True, type=Path, help="developer-supplied PSP ELF (kept private)")
    ap.add_argument("--manifest", type=Path, default=None, help="hle_manifest.json (default: regenerate from source)")
    ap.add_argument("--out", type=Path, default=None, help="write the JSON report here")
    ap.add_argument("--text", type=Path, default=None, help="write the human-readable report here")
    ap.add_argument(
        "--with-addresses",
        action="store_true",
        help="include guest stub addresses (title-specific; keep such reports out of issues and commits)",
    )
    args = ap.parse_args(argv)

    cap = psp_import_table.MAX_FILE_SIZE
    try:
        size = args.elf.stat().st_size
    except OSError as e:
        print(f"import_audit: cannot stat {args.elf}: {e.strerror}", file=sys.stderr)
        return 1
    if size > cap:
        print(f"import_audit: {args.elf} is {size} bytes; refusing inputs over {cap}", file=sys.stderr)
        return 1
    try:
        data = _read_bounded(args.elf, cap)
    except OSError as e:
        print(f"import_audit: cannot read {args.elf}: {e.strerror}", file=sys.stderr)
        return 1
    if data is None:
        print(f"import_audit: {args.elf} exceeds {cap} bytes; refusing", file=sys.stderr)
        return 1
    if args.manifest is not None:
        try:
            manifest = load_manifest(args.manifest)
        except ManifestValidationError as e:
            print(f"import_audit: invalid manifest: {e}", file=sys.stderr)
            return 1
    else:
        from hle_manifest import ManifestError, build_manifest

        try:
            manifest = build_manifest()
        except ManifestError as e:
            print(f"import_audit: {e}", file=sys.stderr)
            return 1
    try:
        table = parse_import_table(data)
    except ImportTableError as e:
        print(f"import_audit: malformed input: {e}", file=sys.stderr)
        return 1

    report = classify_imports(
        table.funcs, manifest, with_addresses=args.with_addresses, findings=table.findings
    )
    text = render_text(report)
    if args.out:
        _dump(report, args.out)
    if args.text:
        _dump(text, args.text)
    if not args.out and not args.text:
        sys.stdout.write(text)
    else:
        s = report["summary"]
        print(
            f"import_audit: {s['total']} imports | dedicated {s['dedicated']}, "
            f"controlled_unsupported {s['controlled_unsupported']}, "
            f"fake_success {s['fake_success']}, missing {s['missing']}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
