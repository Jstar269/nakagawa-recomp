#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Regenerate the waits-matrix registration census (issue #339 acceptance).

Maps every exact ``sce*`` / ``__sce*`` API name named in
``docs/PSP_INTR_WAITS_MATRIX.md`` to one of four exclusive dispositions and a
defaulter owner issue:

    missing               -- no static HLE registration (#339 object-model gap)
    registered            -- registered only as fake success (#281)
    implemented           -- dedicated handler (status from HANDLER_STATUS;
                             ``unreviewed`` matures under #341)
    controlled-unsupported-- deliberate refusal with a documented PSP error

The census is generated from the live ``tools/hle_manifest.build_manifest()``
join, never from a hand-maintained count. ``--check`` re-derives the Markdown
and fails if the committed ``docs/PSP_INTR_WAITS_CENSUS.md`` has drifted.

Globs and call-shaped backticks (``sceIoWaitAsync*``,
``sceKernelWaitSema(sema, 9, NULL)``) are not exact API names and are ignored.

Usage:
    python tools/waits_census.py            # rewrite docs/PSP_INTR_WAITS_CENSUS.md
    python tools/waits_census.py --check    # fail if the committed file is stale
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

from hle_manifest import build_manifest  # noqa: E402

MATRIX = ROOT / "docs" / "PSP_INTR_WAITS_MATRIX.md"
OUT_MD = ROOT / "docs" / "PSP_INTR_WAITS_CENSUS.md"

# Exact identifiers only: a full backtick body that is an sce/__sce symbol.
_API_RE = re.compile(r"^(?:sce|__sce)[A-Za-z0-9_]+$")

# Defaulter owners by exclusive disposition (and maturity for implemented).
OWNER_MISSING = "#339"
OWNER_FAKE_SUCCESS = "#281"
OWNER_UNREVIEWED = "#341"
OWNER_NONE = "-"

DISPOSITIONS = (
    "missing",
    "registered",
    "implemented",
    "controlled-unsupported",
)


def extract_apis(matrix_text: str) -> list[str]:
    """Return sorted unique exact API names named in the matrix Markdown."""
    names: set[str] = set()
    for body in re.findall(r"`([^`]+)`", matrix_text):
        if _API_RE.fullmatch(body):
            names.add(body)
    return sorted(names)


def disposition_owner(row: dict) -> tuple[str, str]:
    """Return (disposition, owner) for one census row."""
    classification = row["classification"]
    status = row["status"]
    if classification == "controlled_unsupported":
        return "controlled-unsupported", OWNER_NONE
    if classification == "fake_success":
        return "registered", OWNER_FAKE_SUCCESS
    # dedicated (or anything else the manifest admits as registered non-stub)
    if status == "unreviewed":
        return "implemented", OWNER_UNREVIEWED
    return "implemented", OWNER_NONE


def build_census(matrix_text: str | None = None, manifest: dict | None = None) -> dict:
    if matrix_text is None:
        matrix_text = MATRIX.read_text(encoding="utf-8")
    if manifest is None:
        manifest = build_manifest()
    by_name: dict[str, list[dict]] = {}
    for reg in manifest["registrations"]:
        by_name.setdefault(reg["name"], []).append(reg)

    rows: list[dict] = []
    for api in extract_apis(matrix_text):
        regs = by_name.get(api, [])
        if not regs:
            rows.append(
                {
                    "api": api,
                    "disposition": "missing",
                    "owner": OWNER_MISSING,
                    "classification": "missing",
                    "status": None,
                    "handler": None,
                    "nid": None,
                }
            )
            continue
        # Deterministic pick when one name maps to multiple NIDs (firmware variants).
        reg = sorted(regs, key=lambda r: r["nid"])[0]
        disposition, owner = disposition_owner(reg)
        rows.append(
            {
                "api": api,
                "disposition": disposition,
                "owner": owner,
                "classification": reg["classification"],
                "status": reg["status"],
                "handler": reg["handler"],
                "nid": reg["nid"],
            }
        )

    summary = {d: 0 for d in DISPOSITIONS}
    owners: dict[str, int] = {}
    for r in rows:
        summary[r["disposition"]] += 1
        if r["owner"] != OWNER_NONE:
            owners[r["owner"]] = owners.get(r["owner"], 0) + 1
    return {
        "schema": 1,
        "source": "docs/PSP_INTR_WAITS_MATRIX.md",
        "manifest_source": "src/rt/hle.c",
        "summary": {
            "total_apis": len(rows),
            "by_disposition": summary,
            "by_owner": dict(sorted(owners.items())),
        },
        "rows": rows,
    }


def render_markdown(census: dict) -> str:
    s = census["summary"]
    by_d = s["by_disposition"]
    lines = [
        "# PSP waits-matrix registration census",
        "",
        "<!-- Generated by tools/waits_census.py. Do not hand-edit. -->",
        "",
        "<!-- markdownlint-disable MD013 -->",
        "",
        "Live join of every exact `sce*` / `__sce*` API named in",
        "`docs/PSP_INTR_WAITS_MATRIX.md` against",
        "`tools/hle_manifest.build_manifest()` (registrations extracted from",
        "`src/rt/hle.c`). Regenerate with `python tools/waits_census.py`;",
        "`--check` fails when this file is stale.",
        "",
        "## Dispositions and owners",
        "",
        "| disposition | meaning | default owner |",
        "| --- | --- | --- |",
        "| `missing` | no static HLE registration | #339 (object-model gap) |",
        "| `registered` | registered only as fake success | #281 |",
        "| `implemented` | dedicated handler; `status` is the semantic maturity | #341 when `unreviewed`, else - |",
        "| `controlled-unsupported` | deliberate refusal with a documented PSP error | - |",
        "",
        "## Summary",
        "",
        f"- total exact APIs: **{s['total_apis']}**",
        f"- missing: **{by_d['missing']}**",
        f"- registered (fake success): **{by_d['registered']}**",
        f"- implemented: **{by_d['implemented']}**",
        f"- controlled-unsupported: **{by_d['controlled-unsupported']}**",
        "",
    ]
    if s["by_owner"]:
        lines.append("Open-owner counts:")
        lines.append("")
        for owner, count in s["by_owner"].items():
            lines.append(f"- {owner}: {count}")
        lines.append("")
    lines.extend(
        [
            "## Census",
            "",
            "| API | disposition | owner | classification | status | handler | nid |",
            "| --- | --- | --- | --- | --- | --- | --- |",
        ]
    )
    for r in census["rows"]:
        status = r["status"] or "-"
        handler = f"`{r['handler']}`" if r["handler"] else "-"
        nid = f"`{r['nid']}`" if r["nid"] else "-"
        classification = r["classification"] or "-"
        lines.append(
            f"| `{r['api']}` | {r['disposition']} | {r['owner']} | "
            f"{classification} | {status} | {handler} | {nid} |"
        )
    lines.append("")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--check",
        action="store_true",
        help="exit 1 if docs/PSP_INTR_WAITS_CENSUS.md differs from a fresh generation",
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=OUT_MD,
        help="output path (default: docs/PSP_INTR_WAITS_CENSUS.md)",
    )
    args = ap.parse_args(argv)

    try:
        text = render_markdown(build_census())
    except Exception as e:  # fail closed on manifest/matrix errors
        print(f"waits_census: {e}", file=sys.stderr)
        return 2

    if args.check:
        if not args.out.is_file():
            print(f"waits_census: missing {args.out}; run tools/waits_census.py", file=sys.stderr)
            return 1
        current = args.out.read_text(encoding="utf-8")
        if current != text:
            print(
                f"waits_census: {args.out} is stale; run python tools/waits_census.py",
                file=sys.stderr,
            )
            return 1
        print(f"waits_census: {args.out} is current")
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8", newline="\n")
    print(f"waits_census: wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
