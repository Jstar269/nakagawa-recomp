# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-08-10.
# See NOTICE.md for upstream lineage and modification provenance.

"""Generate src/rt/nid_names.h from the tracked corpus in tools/nid_corpus.json.

This generator no longer reads a PPSSPP checkout.  It reads a corpus this
repository tracks, and for every entry whose NID is reproducible from its name
it **recomputes** the NID rather than copying one::

    nid = int.from_bytes(sha1(name)[0:4], "little")

That is the derivation ``psp-build-exports`` in the pspdev toolchain applies
when it builds a PRX.  1463 of the emitted NIDs are therefore computed here from
the name, not transcribed from another project's table.

What that does and does not claim
---------------------------------

It removes copied numeric constants.  It does **not** make the name list
independent: those names were seeded from PPSSPP's HLE tables and remain
PPSSPP-suggested, which the corpus records and NOTICE.md continues to state.
The table's classification stays ``derived-data``.  See
``assets/public_provenance_ledger.json`` and ``docs/provenance/INDEPENDENCE_MODEL.md``.

Entries that fail the derivation rule are retained, not deleted -- failing it
means unproven, not wrong.  They store their NID explicitly and carry the class
that says why it could not be derived.  The one exception is sentinel-shaped
entries (reserved ``__`` identifiers, or NIDs like ``0xc0de0001`` that no digest
produces), which are emulator-internal identifiers rather than PSP exports; the
generator refuses to emit them at all.

Usage::

    python tools/gen_nidnames.py            # regenerate the header
    python tools/gen_nidnames.py --check    # verify the header is up to date
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT / "tools" / "nid_corpus.json"
HEADER = ROOT / "src" / "rt" / "nid_names.h"

DERIVABLE_CLASSES = frozenset({"pspsdk-sourced", "hash-verified"})
STORED_CLASSES = frozenset({"library-attributed", "editorial-alias", "unresolved"})
SENTINEL_PREFIXES = ("1337", "c0de", "dead", "beef", "feed")


class CorpusError(RuntimeError):
    """The corpus violated an invariant the generator must not paper over."""


def nid_of(name: str) -> int:
    return int.from_bytes(hashlib.sha1(name.encode()).digest()[:4], "little")


def resolve(entry: dict) -> tuple[int, str]:
    """Return (nid, name), computing the NID when the class says it is derivable."""
    cls = entry.get("class")
    name = entry.get("name")
    if not name:
        raise CorpusError(f"entry without a name: {entry!r}")

    if cls in DERIVABLE_CLASSES:
        if "nid" in entry:
            raise CorpusError(
                f"{name}: class {cls!r} stores a NID, but its whole point is that "
                "the NID is recomputed from the name. Remove the stored value."
            )
        return nid_of(name), name

    if cls in STORED_CLASSES:
        if "nid" not in entry:
            raise CorpusError(f"{name}: class {cls!r} must store its NID")
        nid = int(entry["nid"], 16)
        if nid_of(name) == nid:
            raise CorpusError(
                f"{name}: stored NID 0x{nid:08x} IS reproducible from the name, so "
                f"the entry belongs in a derivable class, not {cls!r}"
            )
        return nid, name

    raise CorpusError(f"{name}: unknown class {cls!r}")


def check_not_sentinel(nid: int, name: str, derivable: bool) -> None:
    """Reject emulator-internal sentinel entries (finding PROV-F5).

    A derivable entry can never be a sentinel: its NID *is* the digest of its
    name, which is precisely what sentinel allocation is not.  Applying the
    heuristic to it would be actively wrong -- the PSP's sceSasCore library
    genuinely exports 33 ``__sceSas*`` names, 32 of which are independently
    confirmed in the pinned PSPSDK headers, and a bare ``__`` prefix test would
    delete every one of them.
    """
    if derivable:
        return
    if name.startswith("__") or f"{nid:08x}".startswith(SENTINEL_PREFIXES):
        raise CorpusError(
            f"refusing to emit sentinel-shaped entry 0x{nid:08x} {name!r}: a reserved "
            "identifier or sentinel-allocated NID that is NOT reproducible from its "
            "name is an emulator-internal identifier, not a PSP export (PROV-F5)"
        )


def render(corpus: dict) -> str:
    entries = corpus["entries"]
    resolved: list[tuple[int, str]] = []
    seen: dict[int, str] = {}
    for entry in entries:
        nid, name = resolve(entry)
        check_not_sentinel(nid, name, entry.get("class") in DERIVABLE_CLASSES)
        if nid in seen:
            raise CorpusError(
                f"duplicate NID 0x{nid:08x}: {seen[nid]!r} and {name!r}"
            )
        seen[nid] = name
        resolved.append((nid, name))
    resolved.sort()

    counts = corpus["counts"]
    derived = sum(counts[c] for c in sorted(DERIVABLE_CLASSES))
    lines = [
        "// SPDX-License-Identifier: GPL-2.0-or-later",
        "// Copyright (C) 2025-2026 the psp-recomp authors",
        "// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)",
        "// Modified by Nakagawa Recomp contributors, 2026-08-10.",
        "// See NOTICE.md for upstream lineage and modification provenance.",
        "",
        "/* NID -> PSP function name table. AUTO-GENERATED by tools/gen_nidnames.py",
        " * from tools/nid_corpus.json. Do not edit by hand.",
        " *",
        " * %d of the %d NIDs below are COMPUTED from their name at generation time"
        % (derived, len(resolved)),
        " * as sha1(name)[0:4] little-endian -- the derivation psp-build-exports applies"
        " when",
        " * building a PRX -- and are not transcribed from any other project's table.",
        " * The remaining %d are not reproducible from their name and are stored"
        % (len(resolved) - derived),
        " * explicitly; the corpus records which class each belongs to and why.",
        " *",
        " * The NAME list is a different question. It was seeded from PPSSPP's Core/HLE",
        " * tables and remains PPSSPP-suggested; no independence is claimed for it. See",
        " * NOTICE.md and assets/public_provenance_ledger.json.",
        " *",
        " * Consulted only on the rare unknown/unimplemented-NID path so a missing import",
        " * names itself instead of printing bare hex. */",
        "#ifndef SR_NID_NAMES_H",
        "#define SR_NID_NAMES_H",
        "#include <stdint.h>",
        "#include <stddef.h>",
        "typedef struct { uint32_t nid; const char *name; } SrNidName;",
        "/* Sorted ascending by nid for binary search. */",
        "static const SrNidName sr_nid_table[] = {",
    ]
    for nid, name in resolved:
        escaped = name.replace("\\", "\\\\").replace('"', '\\"')
        lines.append('    {0x%08xu, "%s"},' % (nid, escaped))
    lines += [
        "};",
        "static const size_t sr_nid_table_count = "
        "sizeof(sr_nid_table)/sizeof(sr_nid_table[0]);",
        "",
        "/* Returns the PSP function name for a NID, or NULL if unknown. */",
        "static inline const char *sr_nid_name(uint32_t nid) {",
        "    size_t lo = 0, hi = sr_nid_table_count;",
        "    while (lo < hi) {",
        "        size_t mid = lo + (hi - lo) / 2;",
        "        uint32_t m = sr_nid_table[mid].nid;",
        "        if (m == nid) return sr_nid_table[mid].name;",
        "        if (m < nid) lo = mid + 1; else hi = mid;",
        "    }",
        "    return NULL;",
        "}",
        "",
        "#endif /* SR_NID_NAMES_H */",
    ]
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--corpus", type=Path, default=CORPUS)
    parser.add_argument("--out", type=Path, default=HEADER)
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail if the generated header differs from the file on disk",
    )
    args = parser.parse_args(argv)

    corpus = json.loads(args.corpus.read_text(encoding="utf-8"))
    if not corpus.get("entries"):
        raise SystemExit(f"refusing to emit an empty table from {args.corpus}")

    try:
        text = render(corpus)
    except CorpusError as exc:
        raise SystemExit(f"gen_nidnames: {exc}") from exc

    if args.check:
        current = args.out.read_text(encoding="utf-8") if args.out.exists() else ""
        if current != text:
            print(
                f"gen_nidnames: {args.out} is stale; rerun tools/gen_nidnames.py",
                file=sys.stderr,
            )
            return 1
        print(f"gen_nidnames: {args.out} is up to date")
        return 0

    args.out.write_text(text, encoding="utf-8", newline="\n")
    derived = sum(corpus["counts"][c] for c in sorted(DERIVABLE_CLASSES))
    print(
        f"wrote {args.out} with {len(corpus['entries'])} entries "
        f"({derived} NIDs computed from their name)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
