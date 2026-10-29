#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Generate the HLE and VFPU verification ledger from the repository's own evidence records.

For every registered HLE NID and every VFPU instruction form the runtime executes,
the ledger records the strongest evidence that the path behaves as on a PSP, as one
of five classes (strongest first):

    hardware-measured  a committed PSP measurement record names the path
    ppsspp-derived     the path's behaviour is taken from PPSSPP, as its source says
    game-route         a qualified title route exercised the path (public record)
    synthetic          only project-built fixtures or selftests exercise the path
    unverified         no recorded evidence

No class is decided here from judgement. Each one is read from a record another
tool or file already owns (see ``CLASS_SOURCES``); the two small curated tables
below only narrow those records, carry a reason per entry, and are validated in
both directions so a stale entry fails the run.

Outputs, both pinned by ``python tools/regen_all.py --check``:

    docs/HLE_COVERAGE.md     the per-NID and per-form table with summary counts
    src/rt/hle_evidence.h    the unverified NIDs, for the runtime's log-once line

    python tools/verification_ledger.py                  # rewrite both outputs
    python tools/verification_ledger.py --out DOC --header HDR
    python tools/verification_ledger.py --json OUT       # machine-readable ledger
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import hle_manifest  # noqa: E402
import hle_registry_meta as meta  # noqa: E402

ROOT = TOOLS.parent
DOC = ROOT / "docs" / "HLE_COVERAGE.md"
HEADER = ROOT / "src" / "rt" / "hle_evidence.h"
FIX_COMMAND = "python tools/regen_all.py"
SCHEMA = 1

CLASSES = ("hardware-measured", "ppsspp-derived", "game-route", "synthetic", "unverified")

CLASS_SOURCES = {
    "hardware-measured": (
        "HLE: a `MEASURED` row of `tools/psp_oracle/manifest.json` whose committed citation names "
        "the API (the `HARDWARE_MEASURED` tier of `tools/hle_manifest.py --evidence-chain`). "
        "VFPU: census evidence tier `H`, or a selftest that compares the form with words a "
        "`MEASURED` oracle row recorded (`VFPU_MEASURED_SELFTESTS`)."
    ),
    "ppsspp-derived": (
        "HLE: PPSSPP is named in the handler's body or in the comment directly above it, in its "
        "`tools/hle_registry_meta.py` notes, or in a block comment that `PPSSPP_BLOCKS` ties to a "
        "family of handlers. VFPU: the form's AOT body calls a kernel from the `src/rt/recomp.c` section derived "
        "from PPSSPP's table-based kernels (`assets/vfpu/PROVENANCE.json`)."
    ),
    "game-route": "No public, title-free record of the NIDs or forms a qualified title route exercised exists yet.",
    "synthetic": (
        "HLE: an executable host test enters the NID (`src/rt/intr_conformance.h` cells or "
        "`src/rt/hle_thread_selftest.c` dispatch), or the handler's notes cite a selftest, a "
        "`tools/test_*.py` module, a Makefile target or a synthetic fixture. VFPU: the census "
        "lists a differential test (synthetic corpus, fuzz or interpreter selftest)."
    ),
    "unverified": "None of the above, or the NID routes to a generic success handler.",
}

#: Handlers whose own comment names PPSSPP, but not as the source of what the handler does.
#: Each entry must match the comment rule; otherwise it is stale and the run fails.
PPSSPP_MENTIONS_NOT_A_SOURCE = {
    "h_TotalFreeMemSize": "the comment quotes a figure PPSSPP reports, for comparison; the accounting is the runtime's own",
    "h_ExitGame": "PPSSPP is cited for the NID's argument signature only, not for what the handler does",
}

#: Block comments in the runtime HLE sources that state a PPSSPP derivation for a whole family
#: of handlers. A block covers the handlers with `prefix` defined after `anchor` and before the
#: next `/* ---- ` section banner. The anchor must occur once, its comment must name PPSSPP, and
#: it must still cover at least one registered handler.
PPSSPP_BLOCKS = (
    {
        "anchor": "sceMpeg (PSMF video): faithful port in src/rt/mpeg.c (from PPSSPP)",
        "prefix": "h_Mpeg",
        "reason": "sceMpeg banner: wrappers over the PPSSPP sceMpeg port in src/rt/mpeg.c",
    },
    {
        "anchor": "Ported semantics from PPSSPP",
        "prefix": "h_Atrac",
        "reason": "sceAtrac block: stream bookkeeping ported from PPSSPP sceAtrac.cpp",
    },
    {
        "anchor": "derived-translated: the handle lifecycle",
        "prefix": "h_Mp3",
        "reason": "sceMp3 block: derived-translated from PPSSPP at a pinned revision",
    },
)

#: Census differential tests that compare a form with words a PSP measured. The census
#: lists them as differential coverage; the cited oracle row is what makes them hardware
#: evidence. Each test must appear in the census and each oracle row must be MEASURED.
VFPU_MEASURED_SELFTESTS = {
    "vfpu-interp-selftest:check_vfpu_forms_measured": {
        "oracle_id": "PSP-VFPU-FORMS-001",
        "reason": "compares the AOT body and the interpreter with the result words the PSP-3000 run recorded",
    },
}

#: The `src/rt/recomp.c` section whose kernels carry the PPSSPP table-kernel derivation.
VFPU_PPSSPP_KERNEL_SECTION = "VFPU transcendental kernels"
RECOMP_C = ROOT / "src" / "rt" / "recomp.c"

#: Dispositions whose words the runtime refuses at the issue #326 boundary: no path executes.
VFPU_REFUSED_DISPOSITIONS = frozenset({"supported-opcode-form-unmodeled", "unsupported-encoding"})

#: Evidence citations in hle_registry_meta that name an executable project-built test.
SYNTHETIC_EVIDENCE_RE = re.compile(
    r"^(?:src/rt/\w*selftest\w*\.c|tools/test_\w+\.py|Makefile|fixtures/(?:display_smoke|production_smoke)/[\w./]+)"
    r"(?=$|[:\s])"
)
#: Evidence citations that describe a hardware run in prose or point at the oracle docs.
HARDWARE_NOTE_RE = re.compile(r"hardware|HARDWARE_ORACLE|psp_oracle", re.IGNORECASE)

_HANDLER_DEF_RE = re.compile(
    r"^(?:static[ \t]+)?uint32_t[ \t]+(h_\w+)[ \t]*\([ \t]*CpuState[ \t]*\*[ \t]*\w*[ \t]*\)[ \t]*\{", re.MULTILINE
)
_BANNER_RE = re.compile(r"^/\* ---- ", re.MULTILINE)
_KERNEL_DEF_RE = re.compile(r"^float[ \t]+(sr_vfpu_\w+)[ \t]*\(", re.MULTILINE)


class LedgerError(RuntimeError):
    """An input the ledger depends on is missing, inconsistent or stale."""


# ---------------------------------------------------------------------------
# Source scanning
# ---------------------------------------------------------------------------


def _banner_name(banner: str) -> str:
    first = banner.split("\n", 1)[0][len("/* ---- ") :]
    name = re.split(r"\s+-{3,}|:|\*/", first, maxsplit=1)[0]
    return name.strip()


def _sections(raw: str) -> list[tuple[int, str, str]]:
    """(start offset, name, banner comment text) of each `/* ---- ` section, in order."""
    out = []
    for m in _BANNER_RE.finditer(raw):
        end = raw.find("*/", m.start())
        banner = raw[m.start() : len(raw) if end < 0 else end + 2]
        out.append((m.start(), _banner_name(banner), banner))
    return out


def _comment_above(lines: list[str], def_line: int) -> str:
    """The comment block that ends on the line directly above a definition, or ''."""
    k = def_line - 1
    if k < 0:
        return ""
    if lines[k].rstrip().endswith("*/"):
        j = k
        while j >= 0 and "/*" not in lines[j]:
            j -= 1
        return "\n".join(lines[max(j, 0) : k + 1])
    j = k
    while j >= 0 and lines[j].lstrip().startswith("//"):
        j -= 1
    return "\n".join(lines[j + 1 : k + 1])


def _body_end(clean: str, open_brace: int) -> int:
    depth = 0
    for i in range(open_brace, len(clean)):
        c = clean[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
    raise LedgerError(f"unbalanced handler body at offset {open_brace}")


def _handler_spans(raw: str) -> list[tuple[str, int, int]]:
    """(handler, definition offset, body end) for each active handler definition in `raw`."""
    active = hle_manifest.active_source(raw)
    clean = hle_manifest._strip_string_literals(active)
    return [(m.group(1), m.start(), _body_end(clean, m.end() - 1)) for m in _HANDLER_DEF_RE.finditer(active)]


def ppsspp_handler_comments(sources: list[tuple[str, str]]) -> dict[str, set[str]]:
    """Handler -> source paths where PPSSPP is named in its body or the comment directly above it.

    A mention above a group of handlers or on a shared helper is not attributed here;
    `PPSSPP_BLOCKS` names the few block comments that cover a whole family.
    """
    found: dict[str, set[str]] = {}
    for path, raw in sources:
        lines = raw.split("\n")
        for name, start, end in _handler_spans(raw):
            if "PPSSPP" in _comment_above(lines, raw.count("\n", 0, start)) or "PPSSPP" in raw[start:end]:
                found.setdefault(name, set()).add(path)
    return found


def ppsspp_block_handlers(
    sources: list[tuple[str, str]],
    registered: set[str],
    blocks: tuple[dict[str, str], ...] = PPSSPP_BLOCKS,
) -> dict[str, list[str]]:
    """Handler -> reasons of the `blocks` that cover it, failing closed on a stale entry."""
    covered: dict[str, list[str]] = {}
    for block in blocks:
        anchor, prefix, reason = block["anchor"], block["prefix"], block.get("reason", "")
        if not reason.strip():
            raise LedgerError(f"PPSSPP_BLOCKS entry {anchor!r} has no reason")
        hits = [(path, raw) for path, raw in sources if anchor in raw]
        if len(hits) != 1 or hits[0][1].count(anchor) != 1:
            raise LedgerError(f"PPSSPP_BLOCKS anchor {anchor!r} must occur exactly once in the runtime HLE sources")
        path, raw = hits[0]
        at = raw.find(anchor)
        opener = raw.rfind("/*", 0, at)
        closer = raw.find("*/", at)
        if opener < 0 or closer < 0 or raw.find("*/", opener, at) >= 0 or "PPSSPP" not in raw[opener:closer]:
            raise LedgerError(f"PPSSPP_BLOCKS anchor {anchor!r} is not inside a comment that names PPSSPP")
        banner = _BANNER_RE.search(raw, closer)
        stop = banner.start() if banner else len(raw)
        names = sorted(
            {name for name, start, _ in _handler_spans(raw) if at < start < stop and name.startswith(prefix)}
            & registered
        )
        if not names:
            raise LedgerError(f"PPSSPP_BLOCKS entry {anchor!r} covers no registered {prefix}* handler in {path}")
        for name in names:
            covered.setdefault(name, []).append(reason)
    return covered


def _meta_text(handler: str) -> str:
    value = meta.HANDLER_METADATA.get(handler)
    if not isinstance(value, dict):
        return ""
    parts: list[str] = []
    for item in value.values():
        parts.extend(item if isinstance(item, list) else [str(item)])
    return "\n".join(parts)


def ppsspp_vfpu_kernels(recomp_text: str) -> tuple[str, ...]:
    """Kernels defined in the recomp.c section that records the PPSSPP table-kernel derivation."""
    sections = _sections(recomp_text)
    for index, (start, name, banner) in enumerate(sections):
        if name != VFPU_PPSSPP_KERNEL_SECTION:
            continue
        if "PPSSPP" not in banner:
            raise LedgerError(f"src/rt/recomp.c section {name!r} no longer names PPSSPP in its banner")
        stop = sections[index + 1][0] if index + 1 < len(sections) else len(recomp_text)
        kernels = tuple(sorted(set(_KERNEL_DEF_RE.findall(recomp_text[start:stop]))))
        if not kernels:
            raise LedgerError(f"src/rt/recomp.c section {name!r} defines no sr_vfpu_* kernel")
        return kernels
    raise LedgerError(f"src/rt/recomp.c has no {VFPU_PPSSPP_KERNEL_SECTION!r} section")


# ---------------------------------------------------------------------------
# Curated-table validation
# ---------------------------------------------------------------------------


def validate_ppsspp_exclusions(
    handlers: set[str],
    mentions: dict[str, set[str]],
    exclusions: dict[str, str] | None = None,
) -> None:
    exclusions = PPSSPP_MENTIONS_NOT_A_SOURCE if exclusions is None else exclusions
    for handler, reason in sorted(exclusions.items()):
        if handler not in handlers:
            raise LedgerError(f"PPSSPP_MENTIONS_NOT_A_SOURCE names {handler!r}, which no registration routes to")
        if not mentions.get(handler):
            raise LedgerError(
                f"PPSSPP_MENTIONS_NOT_A_SOURCE entry {handler!r} is stale: its comment no longer names PPSSPP"
            )
        if not reason.strip():
            raise LedgerError(f"PPSSPP_MENTIONS_NOT_A_SOURCE entry {handler!r} has no reason")


def validate_vfpu_measured_selftests(
    census_tests: set[str],
    oracle: dict,
    table: dict[str, dict[str, str]] | None = None,
) -> None:
    table = VFPU_MEASURED_SELFTESTS if table is None else table
    measured = {t.get("id"): t for t in oracle.get("tests", []) if t.get("hardware_evidence") == "MEASURED"}
    for test, entry in sorted(table.items()):
        if test not in census_tests:
            raise LedgerError(f"VFPU_MEASURED_SELFTESTS names {test!r}, which no census row lists")
        if entry.get("oracle_id") not in measured:
            raise LedgerError(
                f"VFPU_MEASURED_SELFTESTS entry {test!r} cites {entry.get('oracle_id')!r}, "
                "which is not a MEASURED row of tools/psp_oracle/manifest.json"
            )
        if not str(entry.get("reason", "")).strip():
            raise LedgerError(f"VFPU_MEASURED_SELFTESTS entry {test!r} has no reason")


# ---------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------


def classify_hle(chain: dict, mentions: dict[str, set[str]], blocks: dict[str, list[str]]) -> list[dict]:
    subsystems = {r["nid"]: r["subsystem"] for r in chain["_manifest"]["registrations"]}
    rows = []
    for e in chain["entries"]:
        reg = e["registration"]
        handler = reg["handler"]
        ex = e["exercised"]
        ppsspp: list[str] = []
        if mentions.get(handler) and handler not in PPSSPP_MENTIONS_NOT_A_SOURCE:
            ppsspp.append("handler comment names PPSSPP")
        ppsspp += blocks.get(handler, [])
        if "PPSSPP" in _meta_text(handler):
            ppsspp.append("registry notes name PPSSPP")
        tests = [x for x in meta.HANDLER_EVIDENCE.get(handler, []) if SYNTHETIC_EVIDENCE_RE.match(x)]
        if reg["classification"] == "fake_success":
            cls, basis = "unverified", "generic success handler; performs none of the API contract"
        elif e["evidence_tier"] == "HARDWARE_MEASURED":
            cls, basis = "hardware-measured", "PSP oracle " + ", ".join(ex["psp_oracle_probes"])
        elif ppsspp:
            cls, basis = "ppsspp-derived", "; ".join(ppsspp)
        elif e["evidence_tier"] == "HOST_TESTED":
            parts = []
            if ex["conformance_cells"]:
                parts.append("conformance cells")
            if ex["hle_selftest_dispatch"]:
                parts.append("HLE selftest dispatch")
            if not parts:
                # The evidence chain says HOST_TESTED but names no host test: the records disagree.
                raise LedgerError(f"{e['canonical_name']} ({e['nid']}) is HOST_TESTED with no conformance "
                                  "cell or selftest dispatch recorded")
            cls, basis = "synthetic", " and ".join(parts)
        elif tests:
            cls, basis = "synthetic", "registry evidence `" + tests[0].split(" ", 1)[0] + "`"
        else:
            cls, basis = "unverified", "no recorded evidence"
        notes = [x for x in meta.HANDLER_EVIDENCE.get(handler, []) if HARDWARE_NOTE_RE.search(x)]
        rows.append(
            {
                "nid": e["nid"],
                "name": e["canonical_name"],
                "handler": handler,
                "subsystem": subsystems.get(e["nid"]),
                "class": cls,
                "basis": basis,
                "unlinked_hardware_note": bool(notes) and cls != "hardware-measured",
            }
        )
    return sorted(rows, key=lambda r: r["nid"])


def classify_vfpu(census: dict, kernels: tuple[str, ...]) -> tuple[list[dict], int]:
    import vfpu_coverage_report

    kernel_call = re.compile(r"\b(" + "|".join(re.escape(k) for k in kernels) + r")\s*\(")
    rows = []
    refused = 0
    for row in census["rows"]:
        disposition = row["compatibility_disposition"]
        if disposition in VFPU_REFUSED_DISPOSITIONS:
            refused += 1
            continue
        tests = row["differential_tests"]
        measured = sorted(VFPU_MEASURED_SELFTESTS[t]["oracle_id"] for t in tests if t in VFPU_MEASURED_SELFTESTS)
        semantic = sorted({h["oracle_id"] for h in row["hardware_measured"] if h["kind"] == "semantic"})
        body = str(vfpu_coverage_report._classify_aot(int(row["canonical_word"], 16))["aot_emitted"])
        called = sorted(set(kernel_call.findall(body)))
        if row["evidence_tier"] == "H":
            if not semantic:
                # Census tier H claims a hardware measurement the row does not name.
                raise LedgerError(f"VFPU census row {row['canonical_word']} is tier H with no semantic "
                                  "hardware-measured entry")
            cls, basis = "hardware-measured", "census tier H: " + ", ".join(semantic)
        elif measured:
            cls, basis = "hardware-measured", "selftest pinned to " + ", ".join(measured)
        elif called:
            cls, basis = "ppsspp-derived", "calls " + ", ".join(f"`{k}`" for k in called)
        elif tests:
            cls, basis = "synthetic", ", ".join(f"`{t}`" for t in tests)
        else:
            cls, basis = "unverified", "no recorded evidence"
        rows.append(
            {
                "form_id": row["form_id"],
                "opcode": row["opcode"],
                "canonical_word": row["canonical_word"],
                "vector_size": row["vector_size"],
                "disposition": disposition,
                "class": cls,
                "basis": basis,
            }
        )
    return sorted(rows, key=lambda r: r["form_id"]), refused


def build_ledger() -> dict:
    sources = hle_manifest.runtime_hle_sources()
    manifest = hle_manifest.build_manifest()
    chain = hle_manifest.build_evidence_chain(manifest)
    chain["_manifest"] = manifest
    registered = {r["handler"] for r in manifest["registrations"]}
    mentions = ppsspp_handler_comments(sources)
    validate_ppsspp_exclusions(registered, mentions)
    blocks = ppsspp_block_handlers(sources, registered)
    hle_rows = classify_hle(chain, mentions, blocks)

    import vfpu_coverage_report

    census = vfpu_coverage_report.build_census()
    oracle = json.loads(hle_manifest.PSP_ORACLE_MANIFEST.read_text(encoding="utf-8"))
    validate_vfpu_measured_selftests({t for r in census["rows"] for t in r["differential_tests"]}, oracle)
    kernels = ppsspp_vfpu_kernels(RECOMP_C.read_text(encoding="utf-8"))
    vfpu_rows, refused = classify_vfpu(census, kernels)

    cited = {v["oracle_id"] for v in VFPU_MEASURED_SELFTESTS.values()}
    cited |= {h["oracle_id"] for r in census["rows"] for h in r["hardware_measured"]}
    unlinked_vfpu = sorted(
        t["id"]
        for t in oracle.get("tests", [])
        if t.get("hardware_evidence") == "MEASURED" and t.get("id", "").startswith("PSP-VFPU-") and t["id"] not in cited
    )
    for row in (*hle_rows, *vfpu_rows):
        if row["class"] not in CLASSES:
            raise LedgerError(f"unknown evidence class {row['class']!r}")
    return {
        "schema": SCHEMA,
        "classes": list(CLASSES),
        "summary": {
            "hle": {c: sum(1 for r in hle_rows if r["class"] == c) for c in CLASSES},
            "vfpu": {c: sum(1 for r in vfpu_rows if r["class"] == c) for c in CLASSES},
            "vfpu_refused_forms": refused,
        },
        "vfpu_measured_rows_not_linked": unlinked_vfpu,
        "hle": hle_rows,
        "vfpu": vfpu_rows,
    }


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------


def _cell(text: object) -> str:
    return str(text).replace("|", "\\|")


def render_markdown(ledger: dict) -> str:
    hle, vfpu = ledger["summary"]["hle"], ledger["summary"]["vfpu"]
    out = [
        "# HLE and VFPU verification ledger",
        "",
        f"<!-- Generated by tools/verification_ledger.py. Do not hand-edit; regenerate with {FIX_COMMAND}. -->",
        "",
        "For each registered HLE NID and each VFPU instruction form the runtime executes, this page records",
        "the strongest evidence that the path behaves as it does on a PSP. Use it to choose which HLE to",
        "replace with LLE first and to see which paths no evidence backs. A class describes the evidence for",
        "a path, not whether the path is correct.",
        "",
        "## Evidence classes",
        "",
        "Classes are listed strongest first. A path takes the first class whose source names it.",
        "",
        "| Class | Source |",
        "| --- | --- |",
    ]
    out += [f"| `{c}` | {_cell(CLASS_SOURCES[c])} |" for c in CLASSES]
    out += [
        "",
        "## Summary",
        "",
        "| Class | HLE NIDs | VFPU forms |",
        "| --- | ---: | ---: |",
    ]
    out += [f"| `{c}` | {hle[c]} | {vfpu[c]} |" for c in CLASSES]
    out += [
        f"| total | {sum(hle.values())} | {sum(vfpu.values())} |",
        "",
        f"The VFPU census also lists {ledger['summary']['vfpu_refused_forms']} forms the runtime refuses at the",
        "issue #326 boundary (`unsupported-encoding` and `supported-opcode-form-unmodeled`). No path executes",
        "them, so they carry no class; `python tools/vfpu_coverage_report.py` lists them.",
        "",
        "## Limits",
        "",
        "- `game-route` is empty: the repository has no public, title-free record of the NIDs or forms a",
        "  qualified title route exercised. The import-stub census needs private inputs and its output is",
        "  never committed.",
        "- A PPSSPP mention above a group of handlers, or on a shared helper, is not attributed to each",
        "  handler unless `PPSSPP_BLOCKS` in `tools/verification_ledger.py` ties it to a handler family.",
        "  Shared plumbing that every path uses (argument passing, VFPU prefix application) is not",
        "  counted, because it would give every path the same class.",
        "- Hardware runs described only in `tools/hle_registry_meta.py` notes count only when",
        "  `tools/psp_oracle/manifest.json` links them; the rows marked `note` below carry such a note.",
    ]
    if ledger["vfpu_measured_rows_not_linked"]:
        ids = ", ".join(f"`{i}`" for i in ledger["vfpu_measured_rows_not_linked"])
        out += [
            f"- Measured VFPU oracle rows that no census form cites yet: {ids}. Their forms keep the class",
            "  their census row gives until the census links them.",
        ]
    out += [
        "",
        "At run time `sr_syscall` logs one line the first time each `unverified` NID with a handler is",
        "called: `HLE: unverified: <name> (NID 0x<nid>) has no recorded evidence (docs/HLE_COVERAGE.md)`.",
        "Controlled refusals already log their own line once. VFPU forms are not logged: AOT-direct forms",
        "run inline with no shared entry point, and `sr_vfpu_interp` cannot tell which census form a word",
        "belongs to without the census's own grouping.",
        "",
        "## HLE NIDs",
        "",
        "| NID | Name | Subsystem | Class | Basis |",
        "| --- | --- | --- | --- | --- |",
    ]
    for r in ledger["hle"]:
        basis = r["basis"] + ("; note" if r["unlinked_hardware_note"] else "")
        out.append(f"| `{r['nid']}` | `{r['name']}` | {r['subsystem'] or '-'} | `{r['class']}` | {_cell(basis)} |")
    out += [
        "",
        "## VFPU forms",
        "",
        "| Form | Opcode | Word | Size | Disposition | Class | Basis |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for r in ledger["vfpu"]:
        size = "-" if r["vector_size"] is None else str(r["vector_size"])
        out.append(
            f"| `{r['form_id']}` | `{r['opcode']}` | `{r['canonical_word']}` | {size} | "
            f"`{r['disposition']}` | `{r['class']}` | {_cell(r['basis'])} |"
        )
    return "\n".join(out) + "\n"


def render_header(ledger: dict) -> str:
    nids = sorted(int(r["nid"], 16) for r in ledger["hle"] if r["class"] == "unverified")
    out = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// Copyright (C) 2026 the Nakagawa Recomp authors",
        "",
        "/* Registered HLE NIDs whose evidence class is `unverified` (docs/HLE_COVERAGE.md).",
        f" * Generated by tools/verification_ledger.py; do not edit. Regenerate with {FIX_COMMAND}.",
        " * sr_syscall logs the first call of each one; nothing else reads this table. */",
        "#ifndef SR_HLE_EVIDENCE_H",
        "#define SR_HLE_EVIDENCE_H",
        "#include <stdint.h>",
        "",
        f"#define SR_HLE_UNVERIFIED_NID_COUNT {len(nids)}u",
        "/* Sorted ascending for binary search. */",
        f"static const uint32_t sr_hle_unverified_nids[{max(len(nids), 1)}] = {{",
    ]
    out += [f"    0x{n:08x}u," for n in nids] or ["    0u, /* empty: SR_HLE_UNVERIFIED_NID_COUNT is 0 */"]
    out += ["};", "", "#endif", ""]
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=DOC, help="Markdown ledger path")
    parser.add_argument("--header", type=Path, default=HEADER, help="generated runtime header path")
    parser.add_argument("--json", type=Path, default=None, help="also write the ledger as JSON")
    args = parser.parse_args(argv)
    try:
        ledger = build_ledger()
    except (LedgerError, hle_manifest.ManifestError, OSError) as error:
        print(f"verification_ledger: {error}", file=sys.stderr)
        return 1
    except RuntimeError as error:  # vfpu_coverage_report.CensusError and friends
        print(f"verification_ledger: VFPU census: {error}", file=sys.stderr)
        return 1
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(render_markdown(ledger), encoding="utf-8", newline="\n")
    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.header.write_text(render_header(ledger), encoding="utf-8", newline="\n")
    if args.json is not None:
        args.json.write_text(json.dumps(ledger, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
    hle = ", ".join(f"{k}={v}" for k, v in ledger["summary"]["hle"].items())
    vfpu = ", ".join(f"{k}={v}" for k, v in ledger["summary"]["vfpu"].items())
    print(f"verification_ledger: HLE {hle}; VFPU {vfpu} -> {args.out}, {args.header}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
