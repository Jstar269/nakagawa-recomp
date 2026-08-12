#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Independently verify and classify the entries of ``src/rt/nid_names.h``.

PSP firmware exports are addressed by NID, and for the overwhelming majority of
them the NID is a truncated digest of the export name::

    nid == int.from_bytes(sha1(name)[0:4], "little")

That is the rule ``psp-build-exports`` in the pspdev toolchain applies when it
builds a PRX, so a pair satisfying it is a **fact any third party can recompute
from the name alone**.  Re-deriving it here converts most of the shipped table
from "data copied out of another project's source" into data whose correctness
is independently checkable.

What this establishes, precisely
--------------------------------

For a verified entry: *given the name, the NID is independently reproducible.*

It does **not** establish that the list of names is independently sourced, that
the surrounding table structure is original, or that the project was uninfluenced
by the upstream whose tables suggested the names.  See
``assets/public_provenance_ledger.json`` and
``docs/provenance/INDEPENDENCE_MODEL.md`` for the classification that follows
from this measurement.

An entry that fails the rule is **not thereby wrong.**  The PSP has exports whose
names were never published, libraries that may use another derivation, and
editorial aliases that were never export names at all.  Unverified entries are
therefore sorted into observable structural shapes, and anything that does not
fit one is reported as ``unresolved`` rather than guessed at.

Usage::

    python tools/nid_name_proof.py            # human-readable summary
    python tools/nid_name_proof.py --json     # machine-readable report
    python tools/nid_name_proof.py --emit-corpus tools/nid_corpus.json \\
        [--pspsdk-names names.txt]            # regenerate the generator's input

``--emit-corpus`` writes the tracked corpus that ``tools/gen_nidnames.py``
consumes.  Entries whose NID is reproducible carry **no stored NID** -- it is
recomputed at generation time -- so the copied numeric constant is physically
absent from the tracked data.  Sentinel-shaped entries are dropped.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Iterable, NamedTuple

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_HEADER = ROOT / "src" / "rt" / "nid_names.h"

ENTRY_RE = re.compile(r'\{\s*0x(?P<nid>[0-9a-fA-F]{8})u\s*,\s*"(?P<name>[^"]+)"\s*\}')

#: ``<LibraryName>_<8 hex digits>`` where the hex digits are the entry's own NID.
#: The export name is unknown; the entry still records which library owns the NID.
LIBRARY_PLACEHOLDER_RE = re.compile(r"^(?P<library>[A-Za-z0-9_]+)_(?P<hex>[0-9A-Fa-f]{8})$")

#: Editorial composites built by appending a firmware-range suffix to a real
#: export name that is itself present and verifiable in the table.
COMPOSITE_SUFFIX_RE = re.compile(r"^(?P<base>.*?)(?P<suffix>\d{3}(?:_\d{3})?)$")

#: Sentinel NID prefixes no digest realistically produces, used by emulators to
#: allocate internal identifiers in the same numeric space as real NIDs.
SENTINEL_PREFIXES = ("1337", "c0de", "dead", "beef", "feed")

CLASSIFICATIONS = (
    "verified",
    "library-attributed-unknown-name",
    "editorial-alias",
    "emulator-internal-sentinel",
    "unresolved",
)


class Entry(NamedTuple):
    nid: int
    name: str
    classification: str
    rationale: str


def nid_of(name: str) -> int:
    """The documented PSP NID derivation: little-endian SHA-1 prefix."""
    return int.from_bytes(hashlib.sha1(name.encode()).digest()[:4], "little")


def parse_header(text: str) -> list[tuple[int, str]]:
    return [(int(m["nid"], 16), m["name"]) for m in ENTRY_RE.finditer(text)]


def classify(nid: int, name: str, verified_names: set[str]) -> Entry:
    if nid_of(name) == nid:
        return Entry(nid, name, "verified", "nid == sha1(name)[0:4] little-endian")

    placeholder = LIBRARY_PLACEHOLDER_RE.match(name)
    if placeholder and placeholder["hex"].lower() == f"{nid:08x}":
        return Entry(
            nid,
            name,
            "library-attributed-unknown-name",
            f"name encodes the NID itself; records library {placeholder['library']!r} "
            "but no export name",
        )

    if name.startswith("__") or f"{nid:08x}".startswith(SENTINEL_PREFIXES):
        return Entry(
            nid,
            name,
            "emulator-internal-sentinel",
            "reserved identifier and/or sentinel-allocated NID; not digest-derived",
        )

    composite = COMPOSITE_SUFFIX_RE.match(name)
    if composite and composite["base"] in verified_names:
        return Entry(
            nid,
            name,
            "editorial-alias",
            f"firmware-range suffix on {composite['base']!r}, which is itself "
            "present and verifiable in this table",
        )

    return Entry(
        nid,
        name,
        "unresolved",
        "no tested derivation reproduces this NID from this name; the name is "
        "neither confirmed nor refuted",
    )


def classify_all(pairs: Iterable[tuple[int, str]]) -> list[Entry]:
    pairs = list(pairs)
    verified_names = {name for nid, name in pairs if nid_of(name) == nid}
    return [classify(nid, name, verified_names) for nid, name in pairs]


def build_report(entries: list[Entry]) -> dict:
    counts = {c: 0 for c in CLASSIFICATIONS}
    for entry in entries:
        counts[entry.classification] += 1
    return {
        "schema_version": 1,
        "rule": "nid == int.from_bytes(sha1(name)[0:4], 'little')",
        "total": len(entries),
        "counts": counts,
        "entries": [
            {
                "nid": f"0x{e.nid:08x}",
                "name": e.name,
                "classification": e.classification,
                "rationale": e.rationale,
            }
            for e in entries
            if e.classification != "verified"
        ],
    }


#: Classes whose NID is recomputed from the name rather than stored.
DERIVABLE_CLASSES = frozenset({"pspsdk-sourced", "hash-verified"})

#: Corpus classes, in emission order.
CORPUS_CLASSES = (
    "pspsdk-sourced",
    "hash-verified",
    "library-attributed",
    "editorial-alias",
    "unresolved",
)


def build_corpus(entries: list[Entry], pspsdk_names: set[str]) -> dict:
    """Project the classification into the generator's tracked input.

    Sentinel entries are dropped (finding PROV-F5).  Every other entry is kept,
    including the ones that fail the derivation rule: failing it means unproven,
    not wrong.
    """
    verified_names = {e.name for e in entries if e.classification == "verified"}
    out: list[dict] = []
    for entry in entries:
        if entry.classification == "emulator-internal-sentinel":
            continue

        if entry.classification == "verified":
            cls = "pspsdk-sourced" if entry.name in pspsdk_names else "hash-verified"
            out.append({"class": cls, "name": entry.name})
            continue

        record: dict = {
            "class": {
                "library-attributed-unknown-name": "library-attributed",
                "editorial-alias": "editorial-alias",
                "unresolved": "unresolved",
            }[entry.classification],
            "name": entry.name,
            "nid": f"0x{entry.nid:08x}",
        }
        if record["class"] == "library-attributed":
            match = LIBRARY_PLACEHOLDER_RE.match(entry.name)
            if match:
                record["library"] = match["library"]
        elif record["class"] == "editorial-alias":
            composite = COMPOSITE_SUFFIX_RE.match(entry.name)
            if composite and composite["base"] in verified_names:
                record["alias_of"] = composite["base"]
        out.append(record)

    out.sort(key=lambda r: (CORPUS_CLASSES.index(r["class"]), r["name"]))
    counts = {c: sum(1 for r in out if r["class"] == c) for c in CORPUS_CLASSES}
    return {
        "schema_version": 1,
        "description": (
            "Input corpus for tools/gen_nidnames.py. Entries in a derivable "
            "class carry no stored NID: it is recomputed from the name as "
            "sha1(name)[0:4] little-endian at generation time."
        ),
        "derivation_rule": "nid == int.from_bytes(sha1(name)[0:4], 'little')",
        "derivable_classes": sorted(DERIVABLE_CLASSES),
        "provenance": {
            "names": (
                "Seeded from the previously generated src/rt/nid_names.h, whose "
                "name list was scraped from PPSSPP's Core/HLE tables at "
                "f0c28c67446fd9a08b124ea2bfb0e997fe909de5. The names remain "
                "PPSSPP-suggested and this corpus makes no independence claim "
                "about them. What it removes is the copied numeric constant for "
                "every entry whose NID is reproducible from its name."
            ),
            "pspsdk-sourced": (
                "Names additionally present in the pinned PSPSDK headers "
                "(assets/upstream/pspdev.lock.json) and hash-verified, so they "
                "are reproducible with no PPSSPP input at all."
            ),
            "dropped": (
                "Sentinel-shaped entries are excluded; see finding PROV-F5 in "
                "the public provenance ledger and independence model."
            ),
            "regeneration": (
                "python tools/nid_name_proof.py --emit-corpus tools/nid_corpus.json "
                "--pspsdk-names <names.txt>, where <names.txt> is the sorted unique "
                "identifier set of the pinned PSPSDK headers, obtained with: "
                "grep -rhoE '[A-Za-z_][A-Za-z0-9_]{3,}' $PSPDEV/psp/sdk/include "
                "| sort -u. Corpus -> header -> corpus is a verified fixed point. "
                "Omitting --pspsdk-names reclassifies the 840 pspsdk-sourced "
                "entries as hash-verified; it changes no NID and drops no entry."
            ),
        },
        "counts": counts,
        "entries": out,
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--header", type=Path, default=DEFAULT_HEADER)
    parser.add_argument("--json", action="store_true", help="emit the full report as JSON")
    parser.add_argument("--emit-corpus", type=Path, help="write the generator input corpus")
    parser.add_argument(
        "--pspsdk-names",
        type=Path,
        help="newline-separated identifiers from a pinned PSPSDK, used only to "
        "CONFIRM an already-verified pair (never to mint entries)",
    )
    args = parser.parse_args(argv)

    pairs = parse_header(args.header.read_text(encoding="utf-8"))
    if not pairs:
        print(f"nid_name_proof: no entries parsed from {args.header}", file=sys.stderr)
        return 2

    entries = classify_all(pairs)
    report = build_report(entries)

    if args.emit_corpus:
        pspsdk: set[str] = set()
        if args.pspsdk_names:
            pspsdk = {
                line.strip()
                for line in args.pspsdk_names.read_text(
                    encoding="utf-8", errors="ignore"
                ).splitlines()
                if line.strip()
            }
        corpus = build_corpus(entries, pspsdk)
        args.emit_corpus.parent.mkdir(parents=True, exist_ok=True)
        args.emit_corpus.write_text(
            json.dumps(corpus, indent=2, sort_keys=False) + "\n",
            encoding="utf-8",
            newline="\n",
        )
        print(
            f"nid_name_proof: wrote {args.emit_corpus} "
            f"({len(corpus['entries'])} entries, "
            f"{sum(corpus['counts'][c] for c in DERIVABLE_CLASSES)} with derived NIDs)"
        )
        return 0

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0

    counts = report["counts"]
    print(f"nid_name_proof: {report['total']} entries in {args.header}")
    print(f"  rule: {report['rule']}")
    for name in CLASSIFICATIONS:
        print(f"  {name:34s} {counts[name]:5d}")
    verified = counts["verified"]
    print(f"  independently reproducible: {verified}/{report['total']} "
          f"({100.0 * verified / report['total']:.1f}%)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
