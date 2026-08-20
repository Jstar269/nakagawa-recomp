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
    for nid, info in meta.FLOAT_RETURN_NIDS.items():
        if nid not in nids:
            raise ManifestError(f"FLOAT_RETURN_NIDS lists 0x{nid:08x}, which hle.c does not register")
        if not info.get("issue"):
            raise ManifestError(f"FLOAT_RETURN_NIDS entry 0x{nid:08x} has no issue link")
    stale_float_handlers = sorted(meta.FLOAT_RETURN_HANDLERS - handlers)
    if stale_float_handlers:
        raise ManifestError(
            f"FLOAT_RETURN_HANDLERS names handlers hle.c no longer registers: {stale_float_handlers}"
        )


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
        float_info = meta.FLOAT_RETURN_NIDS.get(r["nid"])
        if float_info is not None and r["handler"] not in meta.FLOAT_RETURN_HANDLERS:
            findings.append(
                {
                    "finding": "float_return_handler_mismatch",
                    "nid": r["nid"],
                    "name": r["name"],
                    "handler": r["handler"],
                    "why": (
                        f"{float_info['name']} returns single-precision in $f0; "
                        "an integer-return handler leaves $f0 stale for the guest"
                    ),
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




# ---------------------------------------------------------------------------
# Evidence chain
# ---------------------------------------------------------------------------
# The chain re-derives nothing another tool already owns.  It joins, per NID:
#
#   canonical name -> independently derived NID   tools/nid_name_proof.py
#   imported NID                                  --imports (private retail
#                                                 manifests stay opt-in)
#   registered handler + classification           this module's extraction
#   production dispatch reachability              this module's scope analysis
#   conformance cell / exercised                  src/rt/intr_conformance.h,
#                                                 src/rt/hle_thread_selftest.c
#   hardware exercise                             tools/psp_oracle/manifest.json
#
# Each consumed fact keeps the tier of the tool that produced it.  The chain
# invents no tier of its own, and a link it cannot establish is recorded as
# absent rather than assumed.

CHAIN_SCHEMA = 1

INTR_CONFORMANCE_H = ROOT / "src" / "rt" / "intr_conformance.h"
SELFTEST_C = ROOT / "src" / "rt" / "hle_thread_selftest.c"
PSP_ORACLE_MANIFEST = ROOT / "tools" / "psp_oracle" / "manifest.json"

SELFTEST_MACRO = "SR_HLE_THREAD_SELFTEST"

EVIDENCE_TIERS = (
    "HARDWARE_MEASURED",
    "HOST_TESTED",
    "STATICALLY_SUPPORTED",
    "NOT_EVIDENCE",
)

#: The leading fields of one ``IcProbe`` row: `{ "api", 0xnid, "scenario", ...`
_IC_PROBE_RE = re.compile(
    r'\{\s*"(?P<api>[A-Za-z0-9_]+)"\s*,\s*0x(?P<nid>[0-9a-fA-F]{1,8})[uU]?\s*,\s*"(?P<scenario>[^"]*)"'
)
#: `#define NID_SCE_AUDIO_CH_RESERVE 0x5ec81c55u`
_SELFTEST_NID_DEFINE_RE = re.compile(
    r"^#define[ \t]+(?P<sym>NID_[A-Za-z0-9_]+)[ \t]+0x(?P<nid>[0-9a-fA-F]{1,8})[uU]?[ \t]*$",
    re.MULTILINE,
)
#: `sr_syscall(&cpu, 0x05572a5fu)` -- a NID dispatched as a bare literal.
_SELFTEST_LITERAL_DISPATCH_RE = re.compile(
    r"sr_syscall\s*\([^;)]*,\s*0x(?P<nid>[0-9a-fA-F]{1,8})[uU]?\s*\)"
)
_FUNC_HEAD_RE = re.compile(
    r"^(?:static[ \t]+)?(?:void|int|uint32_t)[ \t]+(?P<name>[A-Za-z_][A-Za-z0-9_]*)[ \t]*\([^;{]*\)[ \t]*\{",
    re.MULTILINE,
)
_HELPER_CALL_RE = re.compile(r"^[ \t]*(?P<name>[A-Za-z_][A-Za-z0-9_]*)[ \t]*\([ \t]*\)[ \t]*;[ \t]*$")


def _read_optional(path: Path) -> str:
    return path.read_text(encoding="utf-8") if path.exists() else ""


def selftest_scope_map(source: str) -> list[str]:
    """Per-line ``SR_HLE_THREAD_SELFTEST`` scope: ``both``/``selftest``/``production``.

    Only a conditional that actually tests the selftest macro narrows the scope.
    Every other conditional leaves it alone, because a registration guarded by an
    unrelated ``#if`` is still compiled into the shipping runtime, and treating
    it as selftest-only would understate production reachability.
    """
    scopes: list[str] = []
    outer: list[str] = []      # scope in effect before each open conditional
    tests_macro: list[bool] = []
    current = "both"
    for line in source.split("\n"):
        m = _PP_IF_RE.match(line)
        if m:
            kind, cond = m.group(1), m.group(2)
            if kind in ("if", "ifdef", "ifndef"):
                macro = SELFTEST_MACRO in cond
                tests_macro.append(macro)
                outer.append(current)
                if macro and current == "both":
                    current = "production" if kind == "ifndef" or cond.lstrip().startswith("!") else "selftest"
            elif kind in ("else", "elif") and tests_macro:
                if tests_macro[-1] and outer[-1] == "both":
                    current = "production" if current == "selftest" else "selftest"
            elif kind == "endif" and tests_macro:
                tests_macro.pop()
                current = outer.pop()
        scopes.append(current)
    return scopes


def enclosing_functions(source: str) -> list[str | None]:
    """Per-line name of the enclosing top-level function, or ``None``."""
    lines = source.split("\n")
    owner: list[str | None] = [None] * len(lines)
    for m in _FUNC_HEAD_RE.finditer(source):
        first = source.count("\n", 0, m.start())
        depth = 0
        for i in range(first, len(lines)):
            owner[i] = m.group("name")
            depth += lines[i].count("{") - lines[i].count("}")
            if depth <= 0:
                break
    return owner


def registration_scopes(raw_source: str) -> dict[int, dict]:
    """Where each registered NID is reachable from: production, selftest, or both.

    A registration inside a helper inherits the scopes of that helper's call
    sites, which is what turns a shared ``hle_register_*_handlers()`` helper into
    evidence that ``sr_hle_init()``'s production branch really reaches it,
    instead of merely evidence that the text exists in the file.
    """
    source = active_source(raw_source)
    # active_source() blanks preprocessor directives, so the conditional scope
    # has to be read from the comment-stripped source, which keeps them. Both
    # views preserve every offset, so line indices agree between them.
    scopes = selftest_scope_map(_strip_comments(raw_source))
    owners = enclosing_functions(source)
    lines = source.split("\n")

    call_scopes: dict[str, set[str]] = {}
    for idx, line in enumerate(lines):
        m = _HELPER_CALL_RE.match(line)
        if m:
            call_scopes.setdefault(m.group("name"), set()).add(scopes[idx])

    def expand(scope: str) -> set[str]:
        return {"production", "selftest"} if scope == "both" else {scope}

    out: dict[int, dict] = {}
    for m in _LITERAL_RE.finditer(source):
        idx = source.count("\n", 0, m.start())
        owner, own_scope = owners[idx], scopes[idx]
        if own_scope != "both" or owner in (None, "sr_hle_init"):
            reach = expand(own_scope)
        else:
            sites = call_scopes.get(owner, set())
            reach = set().union(*(expand(s) for s in sites)) if sites else expand(own_scope)
        out[int(m.group(1), 16)] = {
            "declared_in": owner,
            "reachable_from": sorted(reach),
        }
    return out


def conformance_cells(header_text: str) -> dict[int, list[dict]]:
    """``kIcMatrix`` probe rows grouped by the NID each row dispatches."""
    cells: dict[int, list[dict]] = {}
    parts = header_text.split("static const IcProbe kIcMatrix[]", 1)
    if len(parts) != 2:
        return cells
    for m in _IC_PROBE_RE.finditer(parts[1]):
        cells.setdefault(int(m.group("nid"), 16), []).append(
            {"api": m.group("api"), "scenario": m.group("scenario")}
        )
    return cells


def selftest_dispatched_nids(selftest_text: str) -> set[int]:
    """NIDs the executable HLE selftest enters through ``sr_syscall``.

    Both spellings the file uses are collected: a ``NID_*`` define that is
    referenced elsewhere in the file, and a bare literal handed to sr_syscall.
    A define that is never referenced is deliberately not counted.
    """
    nids: set[int] = set()
    for m in _SELFTEST_NID_DEFINE_RE.finditer(selftest_text):
        if selftest_text.count(m.group("sym")) > 1:
            nids.add(int(m.group("nid"), 16))
    for m in _SELFTEST_LITERAL_DISPATCH_RE.finditer(selftest_text):
        nids.add(int(m.group("nid"), 16))
    return nids


def oracle_exercised_apis(manifest: dict) -> dict[str, list[str]]:
    """API name -> ids of implemented source-owned PSP probes that call it."""
    out: dict[str, list[str]] = {}
    for test in manifest.get("tests", []):
        if test.get("status") != "implemented":
            continue
        for api in test.get("apis", []):
            out.setdefault(api, []).append(test.get("id", "?"))
    return out


def evidence_tier(entry: dict) -> tuple[str, str]:
    """The strongest tier this NID's own links justify, plus the reason.

    Deliberately conservative.  Hardware truth transcribed into the conformance
    matrix describes the PSP, not this runtime, so a conformance cell alone never
    promotes a registration past HOST_TESTED.  Only a source-owned probe that
    actually ran on hardware carries HARDWARE_MEASURED, and that tier describes
    the API exercise, not the correctness of this handler.
    """
    reach = entry["registration"]["reachable_from"]
    ex = entry["exercised"]
    if ex["psp_oracle_probes"]:
        return "HARDWARE_MEASURED", "a source-owned PSP probe calls this API on hardware"
    if ex["conformance_cells"] or ex["hle_selftest_dispatch"]:
        if entry["registration"]["classification"] == "fake_success":
            return "NOT_EVIDENCE", (
                "an executable test enters this NID, but the registered handler is a "
                "generic success stub, so the exercise asserts nothing about the API"
            )
        if "production" in reach:
            return "HOST_TESTED", "an executable test enters the production registration"
        return "HOST_TESTED", "an executable test enters it, but only the selftest build registers it"
    if entry["registration"]["classification"] == "fake_success":
        return "NOT_EVIDENCE", "generic success handler with no executable coverage"
    return "STATICALLY_SUPPORTED", "dedicated handler registered; no executable coverage"


def load_imports(path: Path | None) -> dict[int, str]:
    """Optional imported-NID side of the chain.

    The retail import manifest is a private input, so this is opt-in and absent
    by default; the chain then records the imported link as unknown rather than
    claiming the NID is unimported.
    """
    if path is None:
        return {}
    text = path.read_text(encoding="utf-8")
    found: dict[int, str] = {}
    for m in re.finditer(r'nid\s*=\s*0x([0-9a-fA-F]{1,8}).*?name\s*=\s*"([^"]+)"', text, re.DOTALL):
        found[int(m.group(1), 16)] = m.group(2)
    if not found:
        for m in _LITERAL_RE.finditer(text):
            found[int(m.group(1), 16)] = m.group(2)
    return found



# ---------------------------------------------------------------------------
# Triage
# ---------------------------------------------------------------------------
# Reincorporated from the census concept in PR #76, minus its curated
# per-module weight table. That table is unsourced editorial judgement, and an
# unattested judgement is exactly what a provenance-blocked tool should not
# smuggle in. What survives is the part the chain can already measure: which
# registrations have no executable coverage, how large their module family is,
# and which public tests already name them.

#: `sceKernelFoo` / `__sceSasBar` -> the owning PSP module bucket.
_MODULE_RE = re.compile(r"^(?:__)?(sce[A-Z][a-z0-9]*)")

#: A NID or registered name mentioned by a public test.
PUBLIC_TEST_GLOBS = ("tools/test_*.py", "src/rt/*_selftest.c")


def derive_module(name: str) -> str:
    """Best-effort PSP module bucket. Grouping only -- never a support claim."""
    m = _MODULE_RE.match(name)
    if m:
        return m.group(1)
    if "newlib" in name:
        return "newlib"
    return "other"


def public_test_references(names: set[str], nids: set[str]) -> dict[str, list[str]]:
    """Public test files that mention each registered name or NID literal.

    A mention is a reference, not a test of the API. It is used to rank
    attention, never to claim coverage -- the chain's exercise links are what
    carry that, and they are computed separately.
    """
    refs: dict[str, list[str]] = {}
    for pattern in PUBLIC_TEST_GLOBS:
        for path in sorted(ROOT.glob(pattern)):
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            rel = path.relative_to(ROOT).as_posix()
            for key in names | nids:
                if key in text:
                    refs.setdefault(key, []).append(rel)
    return refs


def triage_candidates(entries: list[dict], top: int) -> list[dict]:
    """Rank registrations that carry no executable evidence.

    Every component is derived from something already in the chain, and each
    one is emitted alongside the score, so a ranking can be argued with rather
    than merely accepted.
    """
    module_counts: dict[str, int] = {}
    for e in entries:
        module_counts[e["module"]] = module_counts.get(e["module"], 0) + 1

    ranked = []
    for e in entries:
        if e["evidence_tier"] not in ("STATICALLY_SUPPORTED", "NOT_EVIDENCE"):
            continue
        family = module_counts.get(e["module"], 0)
        family_norm = min(family / 20.0, 1.0)
        mentions = min(len(e["public_test_references"]), 3)
        # A stub that nothing exercises is worth more attention than a
        # dedicated handler that nothing exercises: it silently reports success.
        stub = 1.0 if e["registration"]["classification"] == "fake_success" else 0.0
        score = family_norm + 0.5 * mentions + stub
        ranked.append({
            "nid": e["nid"],
            "name": e["canonical_name"],
            "handler": e["registration"]["handler"],
            "module": e["module"],
            "evidence_tier": e["evidence_tier"],
            "score": round(score, 3),
            "score_components": {
                "module_family_size": family,
                "family_norm": round(family_norm, 3),
                "public_test_mentions": mentions,
                "unexercised_stub": stub,
            },
            "public_test_references": e["public_test_references"],
        })
    ranked.sort(key=lambda t: (-t["score"], t["nid"]))
    for i, t in enumerate(ranked[:top], start=1):
        t["rank"] = i
    return ranked[:top]


def build_evidence_chain(manifest: dict | None = None, imports_path: Path | None = None,
                         top: int = 30) -> dict:
    """Join every registration to the evidence that does, or does not, back it."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import nid_name_proof

    if manifest is None:
        manifest = build_manifest()
    scopes = registration_scopes(HLE_C.read_text(encoding="utf-8"))
    cells = conformance_cells(_read_optional(INTR_CONFORMANCE_H))
    dispatched = selftest_dispatched_nids(_read_optional(SELFTEST_C))
    try:
        oracle = oracle_exercised_apis(json.loads(_read_optional(PSP_ORACLE_MANIFEST) or "{}"))
    except json.JSONDecodeError:
        oracle = {}
    imports = load_imports(imports_path)
    test_refs = public_test_references(
        {r["name"] for r in manifest["registrations"]},
        {r["nid"] for r in manifest["registrations"]},
    )
    verified_names = {
        r["name"] for r in manifest["registrations"]
        if nid_name_proof.nid_of(r["name"]) == int(r["nid"], 16)
    }

    entries = []
    for reg in manifest["registrations"]:
        nid = int(reg["nid"], 16)
        derived = nid_name_proof.nid_of(reg["name"])
        scope = scopes.get(nid, {"declared_in": None, "reachable_from": []})
        entry = {
            "nid": reg["nid"],
            "canonical_name": reg["name"],
            "module": derive_module(reg["name"]),
            "public_test_references": sorted(
                set(test_refs.get(reg["name"], [])) | set(test_refs.get(reg["nid"], []))
            ),
            "nid_derivation": {
                "independently_derived": derived == nid,
                "method": "nid == sha1(name)[0:4] little-endian",
                "derived_nid": f"0x{derived:08x}",
                # For a label whose NID is not reproducible, defer to
                # nid_name_proof's own shape classification instead of merely
                # flagging it: most are editorial aliases or firmware-suffixed
                # composites, which is a different fact from "wrong".
                "shape": (
                    None if derived == nid
                    else nid_name_proof.classify(nid, reg["name"], verified_names)._asdict()
                ),
            },
            "imported": (
                {"name": imports[nid]} if nid in imports
                else (None if imports else "unknown: no import manifest supplied")
            ),
            "registration": {
                "handler": reg["handler"],
                "origin": reg["origin"],
                "classification": reg["classification"],
                "status": reg["status"],
                "declared_in": scope["declared_in"],
                "reachable_from": scope["reachable_from"],
            },
            "exercised": {
                "conformance_cells": cells.get(nid, []),
                "hle_selftest_dispatch": nid in dispatched,
                "psp_oracle_probes": oracle.get(reg["name"], []),
            },
        }
        entry["exercised_stub"] = bool(
            (entry["exercised"]["conformance_cells"] or entry["exercised"]["hle_selftest_dispatch"])
            and reg["classification"] == "fake_success"
        )
        tier, why = evidence_tier(entry)
        entry["evidence_tier"] = tier
        entry["evidence_rationale"] = why
        entries.append(entry)

    return {
        "schema": CHAIN_SCHEMA,
        "source": manifest["source"],
        "registrations": len(entries),
        "summary": {
            "by_tier": {t: sum(1 for e in entries if e["evidence_tier"] == t) for t in EVIDENCE_TIERS},
            "nid_not_independently_derived": [
                e["nid"] for e in entries if not e["nid_derivation"]["independently_derived"]
            ],
            "by_module": {
                m: sum(1 for e in entries if e["module"] == m)
                for m in sorted({e["module"] for e in entries})
            },
            "not_reachable_from_production": [
                e["nid"] for e in entries if "production" not in e["registration"]["reachable_from"]
            ],
            "exercised_stubs": [
                {"nid": e["nid"], "name": e["canonical_name"], "handler": e["registration"]["handler"]}
                for e in entries if e["exercised_stub"]
            ],
        },
        "triage": triage_candidates(entries, top),
        "entries": entries,
    }


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
    ap.add_argument(
        "--evidence-chain",
        nargs="?",
        const=ROOT / "build" / "hle_evidence_chain.json",
        type=Path,
        default=None,
        help="also emit the per-NID evidence chain (name -> NID -> registration -> "
             "production reachability -> exercise -> tier)",
    )
    ap.add_argument(
        "--triage-top",
        type=int,
        default=30,
        help="how many unexercised registrations to rank in the chain triage list",
    )
    ap.add_argument(
        "--imports",
        type=Path,
        default=None,
        help="optional import manifest supplying the imported-NID link; retail "
             "manifests are private inputs, so the link is unknown without it",
    )
    args = ap.parse_args(argv)
    try:
        manifest = build_manifest()
    except ManifestError as e:
        print(f"hle_manifest: {e}", file=sys.stderr)
        return 1
    dump_json(manifest, args.out)
    print(f"hle_manifest: {len(manifest['registrations'])} registrations -> {args.out}")
    if args.evidence_chain is not None:
        chain = build_evidence_chain(manifest, args.imports, args.triage_top)
        dump_json(chain, args.evidence_chain)
        tiers = ", ".join(f"{k}={v}" for k, v in chain["summary"]["by_tier"].items())
        print(f"hle_manifest: evidence chain -> {args.evidence_chain} ({tiers})")
    if args.write_baseline is not None:
        dump_json(manifest_to_baseline(manifest), args.write_baseline)
        print(f"hle_manifest: baseline -> {args.write_baseline}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
