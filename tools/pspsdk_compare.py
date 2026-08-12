#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Deterministic comparison of PSPSDK declarations and Nakagawa HLE metadata."""

from __future__ import annotations

from collections import Counter, defaultdict
from typing import Any

SCHEMA = 1


def _flatten_upstream(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        function
        for library in manifest["libraries"]
        for function in library["functions"]
    ]


def compare_with_nakagawa(
    upstream_manifest: dict[str, Any],
    nakagawa_manifest: dict[str, Any],
) -> dict[str, Any]:
    by_nid: dict[str, list[dict[str, Any]]] = defaultdict(list)
    by_symbol: dict[str, list[dict[str, Any]]] = defaultdict(list)
    by_pair: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for entry in _flatten_upstream(upstream_manifest):
        by_nid[entry["nid"]].append(entry)
        by_symbol[entry["symbol"]].append(entry)
        by_pair[(entry["nid"], entry["symbol"])].append(entry)

    findings: list[dict[str, Any]] = []
    matched_pairs: set[tuple[str, str]] = set()
    for registration in nakagawa_manifest["registrations"]:
        nid = registration["nid"].lower()
        symbol = registration["name"]
        nid_entries = by_nid.get(nid, [])
        exact = by_pair.get((nid, symbol), [])
        symbol_entries = by_symbol.get(symbol, [])
        base = {
            "nid": nid,
            "nakagawa_symbol": symbol,
            "nakagawa_handler": registration["handler"],
            "nakagawa_classification": registration["classification"],
            "nakagawa_status": registration["status"],
        }
        if exact:
            matched_pairs.add((nid, symbol))
            findings.append(
                {
                    **base,
                    "category": (
                        "exact_pair"
                        if len(nid_entries) == 1
                        else "exact_pair_with_library_ambiguity"
                    ),
                    "upstream": exact,
                    "same_nid_upstream_count": len(nid_entries),
                }
            )
        elif nid_entries:
            findings.append(
                {
                    **base,
                    "category": (
                        "same_nid_conflicting_symbol"
                        if len(nid_entries) == 1
                        else "ambiguous_upstream_nid"
                    ),
                    "upstream": nid_entries,
                }
            )
        elif symbol_entries:
            findings.append(
                {
                    **base,
                    "category": "same_symbol_conflicting_nid",
                    "upstream": symbol_entries,
                }
            )
        else:
            findings.append({**base, "category": "nakagawa_only", "upstream": []})

    for pair, entries in sorted(by_pair.items()):
        if pair not in matched_pairs:
            findings.append(
                {
                    "category": "pspsdk_only",
                    "nid": pair[0],
                    "nakagawa_symbol": None,
                    "nakagawa_handler": None,
                    "nakagawa_classification": None,
                    "nakagawa_status": None,
                    "upstream": entries,
                }
            )

    order = {
        "same_nid_conflicting_symbol": 0,
        "same_symbol_conflicting_nid": 1,
        "ambiguous_upstream_nid": 2,
        "exact_pair_with_library_ambiguity": 3,
        "nakagawa_only": 4,
        "pspsdk_only": 5,
        "exact_pair": 6,
    }
    findings.sort(
        key=lambda item: (
            order.get(item["category"], 99),
            item["nid"],
            item.get("nakagawa_symbol") or "",
        )
    )
    counts = Counter(item["category"] for item in findings)
    return {
        "schema": SCHEMA,
        "upstream_commit": upstream_manifest["upstream"]["commit"],
        "nakagawa_manifest_schema": nakagawa_manifest["schema"],
        "classification_rule": (
            "PSPSDK is declaration evidence only. A mismatch is not automatically "
            "classified as a Nakagawa defect."
        ),
        "counts": dict(sorted(counts.items())),
        "findings": findings,
    }


def _markdown_cell(value: Any) -> str:
    text = str(value).replace("\r", " ").replace("\n", " ")
    return " ".join(text.split()).replace("|", "\\|")


def render_markdown(
    upstream_manifest: dict[str, Any],
    comparison: dict[str, Any],
    *,
    detail_limit: int = 100,
) -> str:
    stats = upstream_manifest["statistics"]
    lines = [
        "# PSPSDK / Nakagawa declaration comparison",
        "",
        f"- PSPSDK commit: `{upstream_manifest['upstream']['commit']}`",
        f"- Source identity: `{upstream_manifest['upstream']['identity']['proof']}`",
        f"- Libraries: **{stats['libraries']}**",
        f"- Imports: **{stats['imports']}**",
        f"- Symbols with prototypes: **{stats['symbols_with_prototypes']}**",
        f"- Conflicting upstream prototypes: **{stats['prototype_conflicts']}**",
        "",
        "PSPSDK is pinned community ABI/declaration evidence, not a hardware",
        "semantic oracle. No mismatch below is automatically a runtime bug.",
        "",
        "## Classification counts",
        "",
        "| Category | Count |",
        "| --- | ---: |",
    ]
    for category, count in comparison["counts"].items():
        lines.append(f"| `{_markdown_cell(category)}` | {count} |")

    interesting = [
        finding
        for finding in comparison["findings"]
        if finding["category"] != "exact_pair"
    ][:detail_limit]
    lines.extend(
        [
            "",
            f"## First {len(interesting)} non-exact findings",
            "",
            "| Category | NID | Nakagawa | PSPSDK declaration(s) |",
            "| --- | --- | --- | --- |",
        ]
    )
    for finding in interesting:
        upstream = ", ".join(
            f"{_markdown_cell(item['library'])}::{_markdown_cell(item['symbol'])}"
            for item in finding["upstream"][:5]
        )
        if len(finding["upstream"]) > 5:
            upstream += f", +{len(finding['upstream']) - 5} more"
        lines.append(
            f"| `{_markdown_cell(finding['category'])}` | "
            f"`{_markdown_cell(finding['nid'])}` | "
            f"`{_markdown_cell(finding.get('nakagawa_symbol') or '-')}` | "
            f"{upstream or '-'} |"
        )
    lines.extend(
        [
            "",
            "## Review rules",
            "",
            "- Investigate mismatches against hardware, PSPAutotests, uOFW, PPSSPP,",
            "  and other primary/independent evidence before changing semantics.",
            "- Preserve Nakagawa implemented/stubbed/controlled-unsupported categories.",
            "- Do not automatically rewrite HLE names, NIDs, handlers, or prototypes.",
            "- Do not include retail/game-derived imports in this report.",
            "",
        ]
    )
    return "\n".join(lines)
