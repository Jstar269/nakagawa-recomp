#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Generate the authoritative HLE-registration manifest from src/rt/hle.c.

This replaces ad hoc regular-expression scraping (tools/nid_auditor.py) with a
fail-closed extraction: every textual occurrence of `sr_hle_register(` in
hle.c must be accounted for as either (a) the function definition, (b) a
fully-parsed literal registration `sr_hle_register(0x…, "name", handler);`, or
(c) the one known dynamic form (the sas_ok[] loop, whose NID array is itself
parsed). Any occurrence that matches none of these is a hard error, so a new
registration style can never be silently dropped from the manifest. Extracted
handler names are cross-checked against handler definitions in the file, and
curated metadata (tools/hle_registry_meta.py) is validated against the
extraction in both directions.

The manifest is deterministic (sorted, ASCII, LF) and carries per-NID
classification so downstream consumers never re-derive policy:

    {"nid": "0x1b4217bc", "name": …, "handler": …, "origin": "static",
     "classification": "fake_success", "status": "stub"}

CLI:
    python tools/hle_manifest.py [--out build/hle_manifest.json]
                                 [--write-baseline tools/import_audit_baseline.json]
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hle_registry_meta as meta  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HLE_C = ROOT / "src" / "rt" / "hle.c"
DEFAULT_BASELINE = ROOT / "tools" / "import_audit_baseline.json"

MANIFEST_SCHEMA = 1

_CALL_TOKEN = "sr_hle_register("
_LITERAL_RE = re.compile(
    r"sr_hle_register\(\s*0x([0-9a-fA-F]{1,8})[uU]?\s*,\s*\"([^\"\\]+)\"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*;"
)
_DEFINITION_RE = re.compile(r"\bvoid\s+sr_hle_register\(")
_SAS_LOOP_RE = re.compile(r"sr_hle_register\(\s*sas_ok\[i\]\s*,\s*\"([^\"]+)\"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*;")
_SAS_ARRAY_RE = re.compile(r"\bsas_ok\[\]\s*=\s*\{([^}]*)\}\s*;", re.DOTALL)
_HANDLER_DEF_RE = re.compile(r"\b(?:static\s+)?uint32_t\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*CpuState\s*\*")
# The one handler-defining macro in hle.c: UTILITY_DIALOG_HANDLERS(h_X, slot)
# expands h_X{Init,Update,Status,Shutdown}.
_DIALOG_MACRO_RE = re.compile(r"^UTILITY_DIALOG_HANDLERS\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,", re.MULTILINE)


class ManifestError(Exception):
    """hle.c contains a registration the extractor cannot prove it captured."""


def _strip_comments(source: str) -> str:
    """Blank out /*...*/ and //... comments, preserving offsets and newlines.

    String and character literals are copied verbatim (escape-aware), so a
    comment opener inside a literal does not start a comment and registration
    name strings survive untouched. Comment bytes become spaces, keeping every
    line number and character offset identical to the input.
    """
    out = list(source)
    i, n = 0, len(source)
    while i < n:
        c = source[i]
        if c in ('"', "'"):
            quote = c
            i += 1
            while i < n and source[i] != quote:
                i += 2 if source[i] == "\\" else 1
            i += 1
        elif c == "/" and i + 1 < n and source[i + 1] == "*":
            end = source.find("*/", i + 2)
            end = n if end < 0 else end + 2
            for j in range(i, end):
                if source[j] != "\n":
                    out[j] = " "
            i = end
        elif c == "/" and i + 1 < n and source[i + 1] == "/":
            end = source.find("\n", i)
            end = n if end < 0 else end
            for j in range(i, end):
                out[j] = " "
            i = end
        else:
            i += 1
    return "".join(out)


_PP_IF_RE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b\s*(.*?)\s*$")


def _blank_inactive_blocks(source: str) -> str:
    """Blank lines inside `#if 0` blocks so disabled registrations stay dead.

    Line-based and deliberately conservative: only a literal `#if 0` opens an
    inactive region (its outermost `#else`/`#elif` branches are treated as
    active, since a possibly-compiled registration must never be omitted);
    every other conditional is treated as active, matching how hle.c is
    actually built. Nested
    conditionals inside an inactive region are tracked so an inner `#endif`
    cannot re-enable it early. Offsets and line numbers are preserved.
    """
    lines = source.split("\n")
    out = []
    # Stack entries: True if the current region is inactive.
    stack: list[bool] = []
    depth_inactive = 0  # nesting depth *inside* the outermost inactive block
    for line in lines:
        m = _PP_IF_RE.match(line)
        inactive = bool(stack and stack[-1])
        if m:
            kind, cond = m.group(1), m.group(2)
            if inactive:
                if kind in ("if", "ifdef", "ifndef"):
                    depth_inactive += 1
                elif kind == "endif":
                    if depth_inactive:
                        depth_inactive -= 1
                    else:
                        stack.pop()
                elif kind in ("else", "elif") and not depth_inactive:
                    # The #else of #if 0 is active; an outermost #elif is
                    # conservatively treated as active too (we cannot evaluate
                    # its condition, and omitting a compiled registration is
                    # the failure mode this scanner must never have).
                    stack[-1] = False
            else:
                if kind in ("if", "ifdef", "ifndef"):
                    stack.append(kind == "if" and cond == "0")
                elif kind == "endif" and stack:
                    stack.pop()
                # #else/#elif of a conditional we cannot evaluate stays
                # active: over-including is fail-closed here, because a
                # phantom entry immediately diverges from the runtime table
                # and trips the baseline/duplicate checks.
            out.append(" " * len(line))  # directives themselves never carry code
            continue
        out.append(" " * len(line) if inactive else line)
    return "\n".join(out)


def active_source(source: str) -> str:
    """Comment-free, `#if 0`-free view of hle.c with identical offsets."""
    return _blank_inactive_blocks(_strip_comments(source))


def classify(handler: str) -> tuple[str, str]:
    """Return (classification, status) for a handler name, from curated policy."""
    status = meta.HANDLER_STATUS.get(handler, "unreviewed")
    if handler in meta.GENERIC_SUCCESS_HANDLERS or status == "stub":
        return "fake_success", "stub"
    if status == "controlled_unsupported":
        return "controlled_unsupported", status
    return "dedicated", status


def extract_registrations(raw_source: str) -> list[dict]:
    """Parse hle.c text into registration dicts, failing closed on anything unaccounted.

    Runs on the active view of the source (comments and `#if 0` regions
    blanked, offsets preserved), so a commented-out or preprocessed-away
    registration can never become a live manifest entry and hex constants
    inside comments can never leak into the sas_ok[] NID set.
    """
    source = active_source(raw_source)
    accounted: set[int] = set()

    regs: list[dict] = []
    for m in _LITERAL_RE.finditer(source):
        accounted.add(m.start())
        nid = int(m.group(1), 16)
        regs.append({"nid": nid, "name": m.group(2), "handler": m.group(3), "origin": "static"})

    for m in _DEFINITION_RE.finditer(source):
        accounted.add(m.end() - len(_CALL_TOKEN))

    sas_loops = list(_SAS_LOOP_RE.finditer(source))
    if sas_loops:
        arrays = list(_SAS_ARRAY_RE.finditer(source))
        if len(sas_loops) != 1 or len(arrays) != 1:
            raise ManifestError(
                f"expected exactly one sas_ok[] array and loop; found {len(arrays)} arrays, {len(sas_loops)} loops"
            )
        accounted.add(sas_loops[0].start())
        sas_name, sas_handler = sas_loops[0].group(1), sas_loops[0].group(2)
        sas_nids = [int(h, 16) for h in re.findall(r"0x[0-9a-fA-F]{1,8}", arrays[0].group(1))]
        if not sas_nids:
            raise ManifestError("sas_ok[] array parsed empty")
        for nid in sas_nids:
            regs.append({"nid": nid, "name": sas_name, "handler": sas_handler, "origin": "sas_ok_loop"})

    unaccounted = []
    start = 0
    while True:
        idx = source.find(_CALL_TOKEN, start)
        if idx < 0:
            break
        # Occurrences inside a C string literal are diagnostics (the
        # unimplemented-NID hint template), not registrations: an odd number
        # of unescaped double quotes precedes the token on its line.
        line_start = source.rfind("\n", 0, idx) + 1
        in_string = len(re.findall(r'(?<!\\)"', source[line_start:idx])) % 2 == 1
        if idx not in accounted and not in_string:
            line = source.count("\n", 0, idx) + 1
            unaccounted.append(f"line {line}: {source[idx: source.find(chr(10), idx)].strip()!r}")
        start = idx + 1
    if unaccounted:
        raise ManifestError(
            "sr_hle_register( occurrences the extractor cannot account for "
            "(teach tools/hle_manifest.py the new form before merging):\n  " + "\n  ".join(unaccounted)
        )

    handlers_defined = set(_HANDLER_DEF_RE.findall(source))
    for m in _DIALOG_MACRO_RE.finditer(source):
        for suffix in ("Init", "Update", "Status", "Shutdown"):
            handlers_defined.add(m.group(1) + suffix)
    missing = sorted({r["handler"] for r in regs} - handlers_defined)
    if missing:
        raise ManifestError(f"registered handlers with no `uint32_t <h>(CpuState *)` definition: {missing}")

    dupes: dict[int, list[str]] = {}
    seen: dict[int, dict] = {}
    for r in regs:
        if r["nid"] in seen:
            dupes.setdefault(r["nid"], [seen[r["nid"]]["name"]]).append(r["name"])
        else:
            seen[r["nid"]] = r
    if dupes:
        detail = ", ".join(f"0x{nid:08x} ({' / '.join(names)})" for nid, names in sorted(dupes.items()))
        raise ManifestError(f"duplicate NID registrations (runtime would reject the later one): {detail}")

    return sorted(regs, key=lambda r: r["nid"])


def validate_meta(regs: list[dict]) -> None:
    """Both-direction cross-check between curated metadata and the extraction."""
    handlers = {r["handler"] for r in regs}
    nids = {r["nid"] for r in regs}
    stale = sorted(set(meta.HANDLER_STATUS) - handlers)
    if stale:
        raise ManifestError(f"HANDLER_STATUS names handlers hle.c no longer registers: {stale}")
    bad_status = sorted(v for v in meta.HANDLER_STATUS.values() if v not in meta.HANDLER_STATUSES)
    if bad_status:
        raise ManifestError(f"HANDLER_STATUS uses unknown status values: {bad_status}")
    for rule in meta.ALIAS_RULES:
        if rule["required_handler"] not in handlers:
            raise ManifestError(f"alias rule requires unknown handler {rule['required_handler']!r}")
    for nid in meta.KNOWN_NID_NAMES:
        if nid not in nids:
            raise ManifestError(f"KNOWN_NID_NAMES lists 0x{nid:08x}, which hle.c does not register")
    for w in meta.WAIVERS:
        if w["nid"] not in nids:
            raise ManifestError(f"waiver for 0x{w['nid']:08x} names an unregistered NID (stale)")
        if not w.get("issue"):
            raise ManifestError(f"waiver for 0x{w['nid']:08x} has no issue link")


def compute_findings(regs: list[dict]) -> list[dict]:
    """Alias-consistency and mislabel findings, before waivers are applied."""
    findings: list[dict] = []
    for r in regs:
        for rule in meta.ALIAS_RULES:
            if r["name"].startswith(rule["name_prefix"]) and r["handler"] != rule["required_handler"]:
                findings.append(
                    {
                        "finding": "alias_mismatch",
                        "nid": r["nid"],
                        "name": r["name"],
                        "handler": r["handler"],
                        "expected_handler": rule["required_handler"],
                        "why": rule["why"],
                    }
                )
        canonical = meta.KNOWN_NID_NAMES.get(r["nid"])
        if canonical is not None and r["name"] != canonical:
            findings.append(
                {
                    "finding": "mislabeled_nid",
                    "nid": r["nid"],
                    "name": r["name"],
                    "handler": r["handler"],
                    "canonical_name": canonical,
                    "why": "registered label disagrees with the canonical NID name",
                }
            )
    return sorted(findings, key=lambda f: (f["nid"], f["finding"]))


def unwaived_and_stale(findings: list[dict]) -> tuple[list[dict], list[dict]]:
    """Split findings/waivers into (unwaived findings, stale waivers)."""
    fkeys = {(f["nid"], f["finding"], f["handler"]) for f in findings}
    wkeys = {(w["nid"], w["finding"], w["handler"]) for w in meta.WAIVERS}
    unwaived = [f for f in findings if (f["nid"], f["finding"], f["handler"]) not in wkeys]
    stale = [dict(w) for w in meta.WAIVERS if (w["nid"], w["finding"], w["handler"]) not in fkeys]
    return unwaived, stale


def build_manifest(source: str | None = None) -> dict:
    if source is None:
        source = HLE_C.read_text(encoding="utf-8")
    regs = extract_registrations(source)
    validate_meta(regs)
    entries = []
    for r in regs:
        classification, status = classify(r["handler"])
        entries.append(
            {
                "nid": f"0x{r['nid']:08x}",
                "name": r["name"],
                "handler": r["handler"],
                "origin": r["origin"],
                "classification": classification,
                "status": status,
            }
        )
    findings = compute_findings(regs)
    return {
        "schema": MANIFEST_SCHEMA,
        "source": "src/rt/hle.c",
        "registrations": entries,
        "findings": [
            {**f, "nid": f"0x{f['nid']:08x}"} for f in findings
        ],
    }


def manifest_to_baseline(manifest: dict) -> dict:
    return {
        r["nid"]: {
            "name": r["name"],
            "handler": r["handler"],
            "classification": r["classification"],
            "status": r["status"],
        }
        for r in manifest["registrations"]
    }


def dump_json(obj: dict, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="ascii", newline="\n") as f:
        json.dump(obj, f, indent=2, sort_keys=True)
        f.write("\n")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "hle_manifest.json")
    ap.add_argument(
        "--write-baseline",
        nargs="?",
        const=DEFAULT_BASELINE,
        type=Path,
        default=None,
        help="refresh the committed classification baseline (review the diff for downgrades)",
    )
    args = ap.parse_args(argv)
    try:
        manifest = build_manifest()
    except ManifestError as e:
        print(f"hle_manifest: {e}", file=sys.stderr)
        return 1
    dump_json(manifest, args.out)
    print(f"hle_manifest: {len(manifest['registrations'])} registrations -> {args.out}")
    if args.write_baseline is not None:
        dump_json(manifest_to_baseline(manifest), args.write_baseline)
        print(f"hle_manifest: baseline -> {args.write_baseline}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
