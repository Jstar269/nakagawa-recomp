#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Compare two builds of a release package and name every difference (#324).

Two clean builds of one commit, one attested binary bundle, one trusted ledger and
one set of prerequisite pins must give the same ZIP bytes
(``docs/PREVIEW_RELEASE.md``, "Check that the package is reproducible"). This tool
is the check. Give it two ZIPs, or two staged folders:

* identical files: prints ``IDENTICAL`` and the SHA-256, exits 0;
* otherwise: prints the differences with their causes (the first 40 of each kind;
  ``--limit`` changes that) and exits 1. For ZIPs the
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


def _raw_stream(path: Path, info: zipfile.ZipInfo) -> str:
    """SHA-256 of an entry's stored (compressed) bytes, read past its local header."""
    with path.open("rb") as stream:
        stream.seek(info.header_offset)
        header = stream.read(30)
        name_length, extra_length = int.from_bytes(header[26:28], "little"), int.from_bytes(header[28:30], "little")
        stream.seek(info.header_offset + 30 + name_length + extra_length)
        digest = hashlib.sha256()
        remaining = info.compress_size
        while remaining:
            chunk = stream.read(min(_CHUNK, remaining))
            if not chunk:
                break
            digest.update(chunk)
            remaining -= len(chunk)
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
            elif left.compress_size != right.compress_size or (
                    left.compress_type != zipfile.ZIP_STORED
                    and _raw_stream(first, left) != _raw_stream(second, right)):
                # Same content: only the Deflate output differs. (A later entry moving because an
                # earlier one changed size is not a difference of its own.)
                note("compressed-bytes", name,
                     f"same content, stored {left.compress_size} vs {right.compress_size} bytes: "
                     "a different zlib or CPython wrote the Deflate stream")
    return found


def _tree(root: Path) -> dict[str, Path]:
    """Files by relative POSIX name, and each empty folder as ``name/`` (the ZIP stores both)."""
    files: dict[str, Path] = {}
    for folder, dirs, names in os.walk(root):
        here = Path(folder)
        if not dirs and not names and here != root:
            files[here.relative_to(root).as_posix() + "/"] = here
        for name in names:
            path = here / name
            files[path.relative_to(root).as_posix()] = path
    return files


def _size(path: Path) -> str:
    return "empty folder" if path.is_dir() else f"{path.stat().st_size} bytes"


def compare_folders(first: Path, second: Path) -> list[dict]:
    """Every file present on one side only or with different bytes."""
    found: list[dict] = []
    a, b = _tree(first), _tree(second)
    for name in sorted(set(a) - set(b)):
        found.append({"kind": "only-in-first", "entry": name, "detail": _size(a[name])})
    for name in sorted(set(b) - set(a)):
        found.append({"kind": "only-in-second", "entry": name, "detail": _size(b[name])})
    for name in sorted(n for n in set(a) & set(b) if not n.endswith("/")):
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
    parser.add_argument("--limit", type=int, default=40,
                        help="differences listed per kind (default 40; 0 lists all); counts are always complete")
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
        listed: dict[str, int] = {}
        counts: dict[str, int] = {}
        for item in report["differences"]:
            counts[item["kind"]] = counts.get(item["kind"], 0) + 1
        for item in report["differences"]:
            kind = item["kind"]
            listed[kind] = listed.get(kind, 0) + 1
            if args.limit and listed[kind] > args.limit:
                continue
            where = f" {item['entry']}:" if item["entry"] else ""
            print(f"  [{kind}]{where} {item['detail']}")
        for kind, count in sorted(counts.items()):
            if args.limit and count > args.limit:
                print(f"  [{kind}] ... {count - args.limit} more ({count} in all); --limit 0 lists them")
    return 0 if report["identical"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
