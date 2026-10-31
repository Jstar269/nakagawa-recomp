#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Compare two builds of a release package and name every difference (#324).

Two clean builds of one commit, one attested binary bundle, one trusted ledger and
one set of prerequisite pins must give the same ZIP bytes
(``docs/PREVIEW_RELEASE.md``, "Check that the package is reproducible"). This tool
is the check. Give it two ZIPs, or two staged folders:

* identical files: prints ``IDENTICAL`` and the SHA-256, exits 0;
* otherwise: prints every difference with its cause and exits 1. For ZIPs the
  causes are the entry list, the entry order, the stored bytes (the unpacked
  content differs), the compressed bytes only (the same content, deflated
  differently: a different zlib or CPython wrote it), the entry time, the
  permission bits, the creating system, extra fields, comments and flags.
  For folders the causes are files present on one side only and files whose
  bytes differ.

A difference found in the ZIPs is traced to a staged file by running the tool on the
two staged folders next. It reads both inputs and writes nothing.

Usage::

    python tools/package_repro_check.py first.zip second.zip
    python tools/package_repro_check.py <staged folder> <staged folder> [--json]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import zipfile
from pathlib import Path

#: ZipInfo fields compared for an entry present in both archives, in report order.
_FIELDS = (
    ("date_time", "entry time"),
    ("external_attr", "permission bits"),
    ("create_system", "creating system"),
    ("create_version", "creator version"),
    ("extract_version", "extractor version"),
    ("flag_bits", "flag bits"),
    ("compress_type", "compression method"),
    ("extra", "extra fields"),
    ("comment", "entry comment"),
)
_CHUNK = 1 << 20


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(_CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _entry_digest(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> str:
    """SHA-256 of the unpacked bytes, read in chunks so a 700 MB entry stays small."""
    digest = hashlib.sha256()
    with archive.open(info) as stream:
        for chunk in iter(lambda: stream.read(_CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def compare_zips(first: Path, second: Path) -> list[dict]:
    """Every difference between two ZIPs, as ``{"kind", "entry", "detail"}`` records."""
    found: list[dict] = []

    def note(kind: str, entry: str, detail: str) -> None:
        found.append({"kind": kind, "entry": entry, "detail": detail})

    with zipfile.ZipFile(first) as one, zipfile.ZipFile(second) as two:
        a = {info.filename: info for info in one.infolist()}
        b = {info.filename: info for info in two.infolist()}
        if len(a) != len(one.infolist()) or len(b) != len(two.infolist()):
            note("duplicate-entry", "", "an archive lists one name twice")
        for name in sorted(set(a) - set(b)):
            note("only-in-first", name, f"{a[name].file_size} bytes")
        for name in sorted(set(b) - set(a)):
            note("only-in-second", name, f"{b[name].file_size} bytes")
        shared = [name for name in a if name in b]
        if shared != [name for name in b if name in a]:
            note("entry-order", "", "the shared entries are stored in a different order")
        if one.comment != two.comment:
            note("archive-comment", "", "the archive comments differ")
        for name in shared:
            left, right = a[name], b[name]
            for attribute, label in _FIELDS:
                if getattr(left, attribute) != getattr(right, attribute):
                    note(attribute, name,
                         f"{label}: {getattr(left, attribute)!r} vs {getattr(right, attribute)!r}")
            if left.file_size != right.file_size or left.CRC != right.CRC:
                note("content", name, f"unpacked {left.file_size} bytes crc {left.CRC:08x} vs "
                                      f"{right.file_size} bytes crc {right.CRC:08x}")
            elif left.compress_size != right.compress_size or left.header_offset != right.header_offset:
                # Same content: only the deflate output (or what precedes it) differs.
                if left.compress_type != zipfile.ZIP_STORED and _entry_digest(one, left) == _entry_digest(two, right):
                    note("compressed-bytes", name,
                         f"same content, stored {left.compress_size} vs {right.compress_size} bytes: "
                         "a different zlib or CPython wrote the Deflate stream")
    return found


def _tree(root: Path) -> dict[str, Path]:
    files: dict[str, Path] = {}
    for folder, _dirs, names in os.walk(root):
        for name in names:
            path = Path(folder) / name
            files[path.relative_to(root).as_posix()] = path
    return files


def compare_folders(first: Path, second: Path) -> list[dict]:
    """Every file present on one side only or with different bytes."""
    found: list[dict] = []
    a, b = _tree(first), _tree(second)
    for name in sorted(set(a) - set(b)):
        found.append({"kind": "only-in-first", "entry": name, "detail": f"{a[name].stat().st_size} bytes"})
    for name in sorted(set(b) - set(a)):
        found.append({"kind": "only-in-second", "entry": name, "detail": f"{b[name].stat().st_size} bytes"})
    for name in sorted(set(a) & set(b)):
        if a[name].stat().st_size != b[name].stat().st_size or sha256_file(a[name]) != sha256_file(b[name]):
            found.append({"kind": "content", "entry": name,
                          "detail": f"{a[name].stat().st_size} vs {b[name].stat().st_size} bytes, SHA-256 differs"})
    return found


def compare(first: Path, second: Path) -> dict:
    """The report for two ZIPs or two folders: ``identical`` and the ``differences``."""
    if first.is_dir() and second.is_dir():
        kind, differences = "folders", compare_folders(first, second)
        identical = not differences
        digests = {}
    elif first.is_file() and second.is_file():
        kind = "zips"
        digests = {"first": sha256_file(first), "second": sha256_file(second)}
        identical = digests["first"] == digests["second"]
        differences = [] if identical else compare_zips(first, second)
        if not identical and not differences:
            differences = [{"kind": "unexplained", "entry": "",
                            "detail": "the files differ but every entry and header matches: "
                                      "padding or data outside the entries"}]
    else:
        raise ValueError("give two ZIP files or two folders")
    return {"kind": kind, "identical": identical, "sha256": digests, "differences": differences}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("first", type=Path)
    parser.add_argument("second", type=Path)
    parser.add_argument("--json", action="store_true", help="print the report as JSON")
    args = parser.parse_args(argv)
    try:
        report = compare(args.first, args.second)
    except (OSError, ValueError, zipfile.BadZipFile) as exc:
        print(f"package_repro_check: FAIL: {exc}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    elif report["identical"]:
        shown = report["sha256"].get("first", "")
        print(f"IDENTICAL {shown}".rstrip())
    else:
        print(f"DIFFERENT: {len(report['differences'])} difference(s)")
        for item in report["differences"]:
            where = f" {item['entry']}:" if item["entry"] else ""
            print(f"  [{item['kind']}]{where} {item['detail']}")
    return 0 if report["identical"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
