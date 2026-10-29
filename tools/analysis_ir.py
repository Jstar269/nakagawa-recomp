# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Advisory analysis IR built from the canonical CFG report (#305).

``codegen.py --cfg-report=<path>`` writes the analyzer's canonical CFG report and
codegen's ``emitted_ownership_map``. This tool projects that report into a
module-wide view for people and review tools: basic blocks with delay slots,
callables, resume entries and continuations, the call graph, cross-references,
and per-instruction guest-register reads/writes and memory accesses. It never
feeds codegen and changes nothing the analyzer decided: every fact in the IR is
derived from the report, ``verify`` proves the IR's ownership matches the report
and codegen's map, and the build leaves the report it reads untouched.

External annotations (Ghidra, decomp.me, manual review, runtime or hardware
observations) carry their source, evidence class and confidence. ``annotate``
compares them with the IR and reports agreements, advisories and disagreements;
an annotation never adds, removes or renames an analyzer fact.

    python tools/analysis_ir.py build --cfg-report report.json --out ir.json
    python tools/analysis_ir.py verify --ir ir.json --cfg-report report.json
    python tools/analysis_ir.py annotate --ir ir.json --annotations notes.json --out report.json
    python tools/analysis_ir.py export-annotations --ir ir.json --out notes.json
    python tools/analysis_ir.py explain --ir ir.json 0x08804068
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

IR_SCHEMA = "nakagawa-analysis-ir-v1"
ANNOTATION_SCHEMA = "nakagawa-analysis-annotations-v1"
ANNOTATION_REPORT_SCHEMA = "nakagawa-analysis-annotation-report-v1"
CFG_SCHEMA_VERSION = 2

ANNOTATION_KINDS = ("function-start", "not-function-start", "function-name", "indirect-targets",
                    "switch-table", "no-return")
EVIDENCE_CLASSES = ("analyzer", "decompiler", "manual", "runtime", "hardware")
CONFIDENCES = ("low", "medium", "high")
# Edge kinds that end a basic block at their source (after the delay slot).
_TRANSFER_KINDS = frozenset({"call", "branch", "direct-branch", "direct-jump", "tail-call",
                             "unresolved-indirect"})
_FUNCTION_CLASSES = ("callable", "resume")


class IRError(ValueError):
    """A report or IR document this tool cannot use, named by a code."""

    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code


# ---------------------------------------------------------------------------
# Guest-register reads and writes of one Allegrex word
# ---------------------------------------------------------------------------

def gpr_effects(word: int) -> dict:
    """GPR reads/writes and memory access of one word, or ``known: False``.

    Only encodings this table names are described; anything else stays unknown
    rather than guessed. r0 never appears: it reads as zero and ignores writes.
    """
    op, rs, rt, rd, funct = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31, word & 63
    reads: set[int] = set()
    writes: set[int] = set()
    mem = None
    known = True
    if op == 0:
        if funct in (0x00, 0x02, 0x03):
            reads, writes = {rt}, {rd}
        elif funct in (0x04, 0x06, 0x07):
            reads, writes = {rs, rt}, {rd}
        elif funct == 0x08:
            reads = {rs}
        elif funct == 0x09:
            reads, writes = {rs}, {rd}
        elif funct in (0x0A, 0x0B):
            reads, writes = {rs, rt, rd}, {rd}      # the old value survives when the move is skipped
        elif funct in (0x0C, 0x0D, 0x0F):
            pass                                    # syscall / break / sync: no direct GPR effect
        elif funct in (0x10, 0x12, 0x16, 0x17):
            reads, writes = ({rs} if funct in (0x16, 0x17) else set()), {rd}
        elif funct in (0x11, 0x13):
            reads = {rs}
        elif funct in (0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x2E, 0x2F):
            reads = {rs, rt}
        elif funct in (0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B, 0x2C, 0x2D):
            reads, writes = {rs, rt}, {rd}
        else:
            known = False
    elif op == 1:
        if rt in (0x00, 0x01, 0x02, 0x03):
            reads = {rs}
        elif rt in (0x10, 0x11, 0x12, 0x13):
            reads, writes = {rs}, {31}
        else:
            known = False
    elif op == 2:
        pass
    elif op == 3:
        writes = {31}
    elif op in (4, 5, 20, 21):
        reads = {rs, rt}
    elif op in (6, 7, 22, 23):
        reads = {rs}
    elif op in (8, 9, 10, 11, 12, 13, 14):
        reads, writes = {rs}, {rt}
    elif op == 15:
        writes = {rt}
    elif op == 0x1F:
        if funct == 0x00:
            reads, writes = {rs}, {rt}
        elif funct == 0x04:
            reads, writes = {rs, rt}, {rt}
        elif funct == 0x20 and ((word >> 6) & 31) in (0x02, 0x03, 0x10, 0x14, 0x18):
            reads, writes = {rt}, {rd}
        else:
            known = False
    elif op in (0x20, 0x21, 0x23, 0x24, 0x25):
        reads, writes = {rs}, {rt}
        mem = {"access": "load", "width": {0x20: 1, 0x24: 1, 0x21: 2, 0x25: 2}.get(op, 4)}
    elif op in (0x22, 0x26):
        reads, writes = {rs, rt}, {rt}
        mem = {"access": "load", "width": 4}
    elif op in (0x28, 0x29, 0x2B, 0x2A, 0x2E):
        reads = {rs, rt}
        mem = {"access": "store", "width": {0x28: 1, 0x29: 2}.get(op, 4)}
    elif op == 0x30:
        reads, writes = {rs}, {rt}
        mem = {"access": "load", "width": 4}
    elif op == 0x38:
        reads, writes = {rs, rt}, {rt}
        mem = {"access": "store", "width": 4}
    elif op in (0x31, 0x32, 0x36):
        reads = {rs}
        mem = {"access": "load", "width": 16 if op == 0x36 else 4}
    elif op in (0x39, 0x3A, 0x3E):
        reads = {rs}
        mem = {"access": "store", "width": 16 if op == 0x3E else 4}
    elif op == 0x2F:
        reads = {rs}
    elif op == 0x11 and rs in (0x00, 0x02):
        writes = {rt}                               # mfc1 / cfc1
    elif op == 0x11 and rs in (0x04, 0x06):
        reads = {rt}                                # mtc1 / ctc1
    else:
        known = False
    if not known:
        return {"known": False, "reads": None, "writes": None, "memory": None}
    return {"known": True, "reads": sorted(reads - {0}), "writes": sorted(writes - {0}), "memory": mem}


# ---------------------------------------------------------------------------
# Building the IR from a CFG report
# ---------------------------------------------------------------------------

def _require_report(report) -> None:
    if not isinstance(report, dict) or report.get("schema_version") != CFG_SCHEMA_VERSION:
        raise IRError("IR_REPORT_SCHEMA", "expected a canonical CFG report, schema_version 2")
    emitted = report.get("emitted_ownership_map")
    if not isinstance(emitted, dict) or not isinstance(emitted.get("regions"), list):
        raise IRError("IR_REPORT_NO_OWNERSHIP_MAP",
                      "the report has no emitted_ownership_map; write it with codegen.py --cfg-report")
    for name in ("instructions", "edges", "entries", "continuation_edges", "interior_entries"):
        if not isinstance(report.get(name), list):
            raise IRError("IR_REPORT_FIELD", f"report field {name!r} is missing or not a list")


def canonical_json(document) -> str:
    return json.dumps(document, ensure_ascii=True, sort_keys=True, separators=(",", ":")) + "\n"


def _sha256(document) -> str:
    return hashlib.sha256(canonical_json(document).encode("ascii")).hexdigest()


def build_ir(report: dict) -> dict:
    """Project a canonical CFG report (with emitted_ownership_map) into the IR."""
    _require_report(report)
    rows = sorted((row for row in report["instructions"] if isinstance(row, dict)),
                  key=lambda row: row["address"])
    by_address = {row["address"]: row for row in rows}
    edges = [edge for edge in report["edges"] if isinstance(edge, dict)]
    owners_of = {row["address"]: tuple(sorted(row.get("owners") or ())) for row in rows}

    functions = {}
    for region in report["emitted_ownership_map"]["regions"]:
        if region.get("classification") not in _FUNCTION_CLASSES:
            continue
        entry = region["start"]
        reasons = []
        for item in by_address.get(entry, {}).get("owner_reasons", ()):
            if item.get("owner") == entry:
                reasons = list(item.get("reasons", ()))
        functions[entry] = {
            "entry": entry,
            "kind": region["classification"],
            "owner": region["owners"][0] if region["classification"] == "resume" else entry,
            "symbol": region.get("symbol"),
            "emitted": bool(region.get("emitted")),
            "entry_reasons": reasons,
            "blocks": [],
            "calls": [],
            "tail_calls": [],
            "unresolved_indirect": [],
            "continuations": [],
            "callers": [],
        }

    # Leaders: entries, branch/jump targets, call returns, interior and continuation
    # targets, the word after each delay slot, and every change of owner set or gap.
    leaders = set(report["entries"])
    leaders.update(item["address"] for item in report["interior_entries"] if isinstance(item, dict))
    leaders.update(edge["target"] for edge in report["continuation_edges"] if isinstance(edge, dict))
    delay_owner = {}
    for edge in edges:
        kind, source, target = edge.get("kind"), edge.get("source"), edge.get("target")
        if kind == "delay-slot" and target is not None:
            delay_owner[target] = source
        if kind in ("branch", "direct-branch", "direct-jump") and target in by_address:
            leaders.add(target)
        if kind == "fallthrough" and edge.get("detail") in ("call-return", "branch-not-taken") \
                and target in by_address:
            leaders.add(target)
    transfer_sources = {edge["source"] for edge in edges if edge.get("kind") in _TRANSFER_KINDS}
    for source in transfer_sources:
        after = source + 8
        if after in by_address:
            leaders.add(after)
    previous = None
    for row in rows:
        address = row["address"]
        if previous is None or address != previous + 4 or owners_of[address] != owners_of[previous]:
            leaders.add(address)
        previous = address

    blocks = []
    block_of = {}
    current = None
    for row in rows:
        address = row["address"]
        if address in leaders or current is None:
            current = {"start": address, "end": address + 4, "owners": list(owners_of[address]),
                       "instructions": [], "successors": []}
            blocks.append(current)
        current["end"] = address + 4
        current["instructions"].append(address)
        block_of[address] = current["start"]

    instructions = []
    for row in rows:
        word = row["word"]
        effect = gpr_effects(word)
        instructions.append({
            "address": row["address"],
            "word": word,
            "mnemonic": (row.get("opcode") or {}).get("mnemonic"),
            "block": block_of[row["address"]],
            "owners": list(owners_of[row["address"]]),
            "classification": row.get("classification"),
            "delay_slot_of": delay_owner.get(row["address"], row.get("delay_slot_of")),
            "branch_likely": bool(row.get("branch_likely")),
            "gpr_known": effect["known"],
            "gpr_reads": effect["reads"],
            "gpr_writes": effect["writes"],
            "memory": effect["memory"],
        })

    blocks_by_start = {block["start"]: block for block in blocks}
    callgraph = []
    xrefs = {}
    for edge in sorted(edges, key=lambda e: (e["source"], -1 if e.get("target") is None else e["target"],
                                             e.get("kind", ""), e.get("detail") or "")):
        kind, source, target = edge.get("kind"), edge["source"], edge.get("target")
        if kind != "delay-slot" and source in block_of:
            block = blocks_by_start[block_of[source]]
            if target is None or block_of.get(target) != block["start"] or target == block["start"]:
                block["successors"].append({"source": source, "target": target, "kind": kind,
                                            "detail": edge.get("detail")})
        if target is not None and kind != "delay-slot":
            xrefs.setdefault(target, []).append({"from": source, "kind": kind, "detail": edge.get("detail")})
        for owner in owners_of.get(source, ()):
            function = functions.get(owner)
            if function is None:
                continue
            if kind == "call":
                function["calls"].append({"site": source, "target": target})
                callgraph.append({"caller": owner, "site": source, "callee": target, "kind": "call"})
            elif kind == "tail-call":
                function["tail_calls"].append({"site": source, "target": target})
                callgraph.append({"caller": owner, "site": source, "callee": target, "kind": "tail-call"})
            elif kind == "unresolved-indirect":
                function["unresolved_indirect"].append(source)
    for row in rows:
        if row["word"] == 0x03E00008:               # jr $ra: the body returns through its link
            blocks_by_start[block_of[row["address"]]]["successors"].append(
                {"source": row["address"], "target": None, "kind": "return", "detail": "jr-ra"})
    for item in report.get("jump_table_ownership", ()) or ():
        if isinstance(item, dict) and type(item.get("target")) is int:
            xrefs.setdefault(item["target"], []).append(
                {"from": item.get("address"), "kind": "jump-table-candidate", "detail": item.get("section")})
    for edge in report["continuation_edges"]:
        function = functions.get(edge.get("owner"))
        if function is not None:
            function["continuations"].append({"source": edge["source"], "target": edge["target"]})
    for block in blocks:
        for owner in block["owners"]:
            if owner in functions:
                functions[owner]["blocks"].append(block["start"])
    for row in callgraph:
        callee = functions.get(row["callee"])
        if callee is not None:
            callee["callers"].append({"caller": row["caller"], "site": row["site"], "kind": row["kind"]})

    return {
        "schema": IR_SCHEMA,
        "advisory": True,
        "source": {
            "cfg_schema_version": report["schema_version"],
            "cfg_report_sha256": _sha256(report),
            "emitted_ownership_schema_version": report["emitted_ownership_map"].get("schema_version"),
        },
        "evidence": {"facts": "analyzer", "note": "derived from the canonical CFG report; not used by codegen"},
        "executable_intervals": report.get("executable_intervals", []),
        "functions": [functions[entry] for entry in sorted(functions)],
        "blocks": blocks,
        "instructions": instructions,
        "callgraph": callgraph,
        "xrefs": [{"target": target, "refs": refs} for target, refs in sorted(xrefs.items())],
        "interior_entries": report["interior_entries"],
        "continuations": report["continuation_edges"],
        "unowned_executable": report.get("unowned_executable", []),
        "ownership_conflicts": report.get("ownership_conflicts", []),
        "jump_table_candidates": report.get("jump_table_ownership", []),
        "summary": {
            "functions": len(functions),
            "callable": sum(1 for f in functions.values() if f["kind"] == "callable"),
            "resume": sum(1 for f in functions.values() if f["kind"] == "resume"),
            "blocks": len(blocks),
            "instructions": len(instructions),
            "gpr_unknown": sum(1 for i in instructions if not i["gpr_known"]),
            "call_edges": sum(1 for r in callgraph if r["kind"] == "call"),
            "tail_call_edges": sum(1 for r in callgraph if r["kind"] == "tail-call"),
            "continuations": len(report["continuation_edges"]),
        },
    }


def load_ir(text: str) -> dict:
    try:
        ir = json.loads(text)
    except json.JSONDecodeError as exc:
        raise IRError("IR_JSON", str(exc)) from None
    if not isinstance(ir, dict) or ir.get("schema") != IR_SCHEMA:
        raise IRError("IR_SCHEMA", f"expected schema {IR_SCHEMA}")
    for name in ("functions", "blocks", "instructions", "callgraph", "xrefs"):
        if not isinstance(ir.get(name), list):
            raise IRError("IR_FIELD", f"IR field {name!r} is missing or not a list")
    return ir


def ownership_parity(ir: dict, report: dict) -> list[dict]:
    """Differences between the IR's ownership and the report it came from (empty = parity)."""
    _require_report(report)
    findings = []
    expected = {(r["start"], r["classification"]) for r in report["emitted_ownership_map"]["regions"]
                if r.get("classification") in _FUNCTION_CLASSES}
    observed = {(f["entry"], f["kind"]) for f in ir["functions"]}
    for entry, kind in sorted(expected - observed):
        findings.append({"code": "function-missing", "address": entry, "kind": kind})
    for entry, kind in sorted(observed - expected):
        findings.append({"code": "function-extra", "address": entry, "kind": kind})
    report_owners = {row["address"]: sorted(row.get("owners") or ()) for row in report["instructions"]}
    block_owners = {b["start"]: b["owners"] for b in ir["blocks"]}
    covered = set()
    for insn in ir["instructions"]:
        address = insn["address"]
        covered.add(address)
        if insn["owners"] != report_owners.get(address):
            findings.append({"code": "instruction-owners", "address": address})
        if block_owners.get(insn["block"]) != insn["owners"]:
            findings.append({"code": "block-owners", "address": address})
    for address in sorted(set(report_owners) - covered):
        findings.append({"code": "instruction-missing", "address": address})
    ir_cont = sorted((c["source"], c["target"], c["owner"]) for c in ir["continuations"])
    rep_cont = sorted((c["source"], c["target"], c["owner"]) for c in report["continuation_edges"])
    if ir_cont != rep_cont:
        findings.append({"code": "continuations", "address": None})
    for function in ir["functions"]:
        owner_blocks = {b["start"] for b in ir["blocks"] if function["entry"] in b["owners"]}
        if set(function["blocks"]) != owner_blocks:
            findings.append({"code": "function-blocks", "address": function["entry"]})
    if ir.get("source", {}).get("cfg_report_sha256") != _sha256(report):
        findings.append({"code": "source-hash", "address": None})
    return findings


# ---------------------------------------------------------------------------
# External annotations
# ---------------------------------------------------------------------------

def load_annotations(document) -> list[dict]:
    if not isinstance(document, dict) or document.get("schema") != ANNOTATION_SCHEMA:
        raise IRError("ANNOTATION_SCHEMA", f"expected schema {ANNOTATION_SCHEMA}")
    rows = document.get("annotations")
    if not isinstance(rows, list):
        raise IRError("ANNOTATION_FIELD", "annotations must be a list")
    clean = []
    for index, row in enumerate(rows):
        where = f"annotation {index}"
        if not isinstance(row, dict):
            raise IRError("ANNOTATION_ROW", f"{where} is not an object")
        if type(row.get("address")) is not int or not 0 <= row["address"] <= 0xFFFFFFFF:
            raise IRError("ANNOTATION_ROW", f"{where}: address must be a 32-bit integer")
        if row.get("kind") not in ANNOTATION_KINDS:
            raise IRError("ANNOTATION_ROW", f"{where}: kind must be one of {', '.join(ANNOTATION_KINDS)}")
        if not isinstance(row.get("source"), str) or not row["source"].strip():
            raise IRError("ANNOTATION_ROW", f"{where}: source is required")
        if row.get("evidence_class") not in EVIDENCE_CLASSES:
            raise IRError("ANNOTATION_ROW", f"{where}: evidence_class must be one of {', '.join(EVIDENCE_CLASSES)}")
        if row.get("confidence") not in CONFIDENCES:
            raise IRError("ANNOTATION_ROW", f"{where}: confidence must be one of {', '.join(CONFIDENCES)}")
        if row["kind"] == "function-name" and not isinstance(row.get("name"), str):
            raise IRError("ANNOTATION_ROW", f"{where}: function-name needs a name")
        if row["kind"] in ("indirect-targets", "switch-table"):
            targets = row.get("targets")
            if not isinstance(targets, list) or not targets or any(type(t) is not int for t in targets):
                raise IRError("ANNOTATION_ROW", f"{where}: {row['kind']} needs a list of integer targets")
        clean.append(row)
    return clean


def apply_annotations(ir: dict, annotations: list[dict]) -> dict:
    """Compare annotations with the IR. The IR is not modified."""
    functions = {f["entry"]: f for f in ir["functions"]}
    insns = {i["address"]: i for i in ir["instructions"]}
    incoming = {row["target"]: row["refs"] for row in ir["xrefs"]}
    unresolved = {site for f in ir["functions"] for site in f["unresolved_indirect"]}
    agreements, advisories, disagreements = [], [], []

    def verdict(bucket, row, analyzer, note):
        bucket.append({"annotation": row, "analyzer": analyzer, "note": note})

    for row in annotations:
        address, kind = row["address"], row["kind"]
        insn = insns.get(address)
        function = functions.get(address)
        if kind == "function-start":
            if function is not None:
                verdict(agreements, row, f"{function['kind']} entry", "the analyzer has this entry")
            elif insn is None:
                verdict(disagreements, row, "not analyzed code", "the address is not an analyzed instruction")
            elif insn["owners"]:
                owners = ", ".join(f"0x{o:08x}" for o in insn["owners"])
                verdict(disagreements, row, f"interior of {owners}",
                        "the analyzer keeps this word inside another body; the annotation is not adopted")
            else:
                verdict(advisories, row, "unowned word", "no analyzer owner; recorded as advice only")
        elif kind == "not-function-start":
            if function is not None:
                verdict(disagreements, row, f"{function['kind']} entry",
                        "the analyzer emits an entry here; the annotation is not adopted")
            else:
                verdict(agreements, row, "no entry", "the analyzer has no entry here")
        elif kind == "function-name":
            if function is None:
                verdict(disagreements, row, "no entry", "a name for an address the analyzer does not enter")
            elif function.get("symbol") and function["symbol"].endswith(row["name"]):
                verdict(agreements, row, function["symbol"], "the emitted symbol already carries this name")
            else:
                verdict(advisories, row, function.get("symbol"), "a name the build does not use; advice only")
        elif kind in ("indirect-targets", "switch-table"):
            outside = [t for t in row["targets"] if t not in insns]
            if insn is None:
                verdict(disagreements, row, "not analyzed code", "the transfer site is not an analyzed instruction")
            elif outside:
                verdict(disagreements, row, "targets outside analyzed code",
                        "targets " + ", ".join(f"0x{t:08x}" for t in outside) + " are not analyzed instructions")
            elif address in unresolved:
                verdict(advisories, row, "unresolved indirect transfer",
                        "the analyzer leaves this transfer to the dispatcher; advice only")
            else:
                known = sorted({r["target"] for r in ir["xrefs"]
                                for ref in r["refs"] if ref["from"] == address})
                if set(row["targets"]) <= set(known):
                    verdict(agreements, row, "resolved transfer", "every annotated target is an analyzer edge")
                else:
                    verdict(advisories, row, "resolved transfer", "extra targets are advice only")
        elif kind == "no-return":
            callers = [ref for ref in incoming.get(address, ()) if ref["kind"] == "call"]
            if function is None:
                verdict(disagreements, row, "no entry", "no-return on an address the analyzer does not enter")
            else:
                verdict(advisories, row, f"{len(callers)} call sites",
                        "the analyzer keeps the call-return fall-through; advice only")
    measured = [d for d in disagreements if d["annotation"]["evidence_class"] in ("runtime", "hardware")]
    return {
        "schema": ANNOTATION_REPORT_SCHEMA,
        "ir_sha256": _sha256(ir),
        "counts": {"agreements": len(agreements), "advisories": len(advisories),
                   "disagreements": len(disagreements), "measured_disagreements": len(measured)},
        "agreements": agreements,
        "advisories": advisories,
        "disagreements": disagreements,
    }


def export_annotations(ir: dict, source: str = "nakagawa-analyzer") -> dict:
    """The analyzer's entries and names as annotations (for another tool to import)."""
    rows = []
    for function in ir["functions"]:
        rows.append({"address": function["entry"], "kind": "function-start", "source": source,
                     "evidence_class": "analyzer", "confidence": "high",
                     "note": f"{function['kind']}; reasons: {', '.join(function['entry_reasons']) or 'none'}"})
        if function.get("symbol"):
            rows.append({"address": function["entry"], "kind": "function-name", "name": function["symbol"],
                         "source": source, "evidence_class": "analyzer", "confidence": "high"})
    return {"schema": ANNOTATION_SCHEMA, "annotations": rows}


# ---------------------------------------------------------------------------
# Explaining one address
# ---------------------------------------------------------------------------

def explain(ir: dict, address: int, annotation_report: dict | None = None) -> list[str]:
    insns = {i["address"]: i for i in ir["instructions"]}
    functions = {f["entry"]: f for f in ir["functions"]}
    lines = [f"0x{address:08x}"]
    insn = insns.get(address)
    if insn is None:
        lines.append("  not an analyzed instruction")
    else:
        lines.append(f"  word 0x{insn['word']:08x} {insn['mnemonic']}, block 0x{insn['block']:08x}, "
                     f"classification {insn['classification']}")
        if insn["delay_slot_of"] is not None:
            lines.append(f"  delay slot of 0x{insn['delay_slot_of']:08x}")
        if insn["gpr_known"]:
            lines.append(f"  reads {insn['gpr_reads'] or '-'}, writes {insn['gpr_writes'] or '-'}"
                         + (f", {insn['memory']['access']} {insn['memory']['width']} bytes" if insn["memory"] else ""))
        for owner in insn["owners"] or []:
            f = functions.get(owner)
            what = f"{f['kind']} {f['symbol'] or ''}".strip() if f else "owner not emitted as an entry"
            lines.append(f"  owned by 0x{owner:08x} ({what})")
        if not insn["owners"]:
            lines.append("  no owner: the analyzer reached no control flow into this word")
    function = functions.get(address)
    if function is not None:
        lines.append(f"  {function['kind']} entry; reasons: {', '.join(function['entry_reasons']) or 'none'}; "
                     f"{len(function['blocks'])} blocks, {len(function['calls'])} calls, "
                     f"{len(function['unresolved_indirect'])} unresolved indirect transfers")
    for row in ir["xrefs"]:
        if row["target"] == address:
            for ref in row["refs"]:
                origin = "?" if ref["from"] is None else f"0x{ref['from']:08x}"
                lines.append(f"  reached from {origin} by {ref['kind']}" + (f" ({ref['detail']})" if ref["detail"] else ""))
    for item in ir["interior_entries"]:
        if item.get("address") == address:
            lines.append(f"  interior entry from 0x{item['source']:08x} into owner 0x{item['owner']:08x}")
    for item in ir["continuations"]:
        if item.get("target") == address:
            lines.append(f"  continuation target from 0x{item['source']:08x} (owner 0x{item['owner']:08x})")
    if any(address in f["unresolved_indirect"] for f in ir["functions"]):
        lines.append("  unresolved indirect transfer: the runtime dispatcher decides the target")
    if annotation_report is not None:
        for bucket, label in (("agreements", "agreement"), ("advisories", "advisory"),
                              ("disagreements", "disagreement")):
            for item in annotation_report.get(bucket, ()):
                row = item["annotation"]
                if row["address"] == address:
                    lines.append(f"  annotation ({label}): {row['kind']} from {row['source']} "
                                 f"[{row['evidence_class']}, {row['confidence']}]: {item['note']}")
    return lines


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------

def _read_json(path: str):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise IRError("IR_INPUT", f"{path}: {exc}") from None


def _write(path: str | None, text: str) -> None:
    if path is None:
        sys.stdout.write(text)
    else:
        Path(path).write_text(text, encoding="ascii", newline="\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    b = sub.add_parser("build", help="build the IR from a codegen --cfg-report file")
    b.add_argument("--cfg-report", required=True)
    b.add_argument("--out")
    v = sub.add_parser("verify", help="check the IR's ownership against its CFG report")
    v.add_argument("--ir", required=True)
    v.add_argument("--cfg-report", required=True)
    a = sub.add_parser("annotate", help="compare external annotations with the IR")
    a.add_argument("--ir", required=True)
    a.add_argument("--annotations", required=True)
    a.add_argument("--out")
    e = sub.add_parser("export-annotations", help="write the analyzer's entries and names as annotations")
    e.add_argument("--ir", required=True)
    e.add_argument("--out")
    x = sub.add_parser("explain", help="explain one address")
    x.add_argument("--ir", required=True)
    x.add_argument("--annotation-report")
    x.add_argument("address")
    args = parser.parse_args(argv)
    try:
        if args.command == "build":
            ir = build_ir(_read_json(args.cfg_report))
            _write(args.out, canonical_json(ir))
            s = ir["summary"]
            sys.stderr.write(f"ANALYSIS_IR functions={s['functions']} blocks={s['blocks']} "
                             f"instructions={s['instructions']} gpr_unknown={s['gpr_unknown']}\n")
        elif args.command == "verify":
            ir = load_ir(Path(args.ir).read_text(encoding="utf-8"))
            findings = ownership_parity(ir, _read_json(args.cfg_report))
            for finding in findings:
                sys.stdout.write(json.dumps(finding, sort_keys=True) + "\n")
            sys.stdout.write(f"ANALYSIS_IR_PARITY {'PASS' if not findings else 'FAIL'} findings={len(findings)}\n")
            return 0 if not findings else 1
        elif args.command == "annotate":
            ir = load_ir(Path(args.ir).read_text(encoding="utf-8"))
            before = canonical_json(ir)
            report = apply_annotations(ir, load_annotations(_read_json(args.annotations)))
            if canonical_json(ir) != before:
                raise IRError("IR_MUTATED", "annotation comparison changed the IR")
            _write(args.out, canonical_json(report))
            c = report["counts"]
            sys.stderr.write(f"ANALYSIS_ANNOTATIONS agreements={c['agreements']} advisories={c['advisories']} "
                             f"disagreements={c['disagreements']} measured_disagreements={c['measured_disagreements']}\n")
        elif args.command == "export-annotations":
            ir = load_ir(Path(args.ir).read_text(encoding="utf-8"))
            _write(args.out, canonical_json(export_annotations(ir)))
        elif args.command == "explain":
            ir = load_ir(Path(args.ir).read_text(encoding="utf-8"))
            report = _read_json(args.annotation_report) if args.annotation_report else None
            sys.stdout.write("\n".join(explain(ir, int(args.address, 0), report)) + "\n")
    except IRError as exc:
        sys.stderr.write(f"{exc}\n")
        return 2
    return 0


__all__ = ["IRError", "apply_annotations", "build_ir", "canonical_json", "explain", "export_annotations",
           "gpr_effects", "load_annotations", "load_ir", "ownership_parity"]

if __name__ == "__main__":
    raise SystemExit(main())
