#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Carry a title manifest's ``runtime_bindings`` to another revision of the same game (#1847).

A manifest's ``runtime_bindings`` hold addresses in one analysed executable. Another
revision moves functions, so those addresses go stale. This tool finds each bound
address's function in the new revision by comparing code with addresses masked, says
how every one was matched, and refuses to guess: anything it cannot match with
confidence is reported by name and left out of the proposed block.

    python tools/binding_carry.py --old-cfg old_cfg.json --new-cfg new_cfg.json \\
        --manifest assets/titles/<title>.json [--accept-fuzzy] [--out-bindings new.json] [--report report.json]

Inputs are two canonical CFG reports (``codegen.py --cfg-report``, schema 2), or two
analysis IR files (``tools/analysis_ir.py build``). An optional ``--old-imports`` /
``--new-imports`` file maps an import-stub address to its NID (``{"0x08804000": "0x1234ABCD"}``)
so stubs match by NID, never by their identical bodies. The manifest is only read: the
proposal goes to stdout or ``--out-bindings`` and the report to ``--report``.

Masking (one token per instruction of a function's own span, in address order)
------------------------------------------------------------------------------
- ``jal``/``j`` out of the function: the callee's identity (an import by NID, otherwise
  masked); a ``j`` or branch inside the function keeps its offset from the entry.
- ``lui`` keeps its register and drops the immediate; ``addiu``/``addi``/``ori`` and any
  load or store whose base register holds a ``lui`` result or is ``$gp`` drop their offset.
- Everything else, including the delay slots, is kept verbatim.

The ladder, strongest first
---------------------------
1. ``exact``: one old and one new function have the same masked code.
2. ``neighbour``: several functions share masked code; the call sites already matched and
   the nearest matched functions before and after them tell the copies apart, but only
   when the evidence picks one pair and nothing ties.
3. ``fuzzy``: a changed body, accepted at 0.75 similarity or more, clearly ahead of the
   runner-up, and chosen by exactly one old function. Needs ``--accept-fuzzy`` to be written.
4. ``caller``: a still-unmatched function that every exactly matched caller calls at the
   same position. Needs ``--accept-fuzzy`` to be written.

Otherwise the function is ``ambiguous`` (several candidates, all listed) or ``unmatched``
(with its closest candidate and similarity). A bound address that is not a function entry
is carried only when the two bodies align token for token at that address.

The analysis inputs are derived from a title and so is the report: keep both private
(AGENTS.md section 5). The tool never reads the network or a game file.
"""

from __future__ import annotations

import argparse
import collections
import difflib
import hashlib
import json
from pathlib import Path
import sys
from typing import Any

_TOOLS_DIRECTORY = str(Path(__file__).resolve().parent)
if _TOOLS_DIRECTORY not in sys.path:
    sys.path.insert(0, _TOOLS_DIRECTORY)

import analysis_ir
import title_manifest

#: Fuzzy similarity needed, the gap to the runner-up, and the shortest body worth comparing.
FUZZY_MIN_SIMILARITY = 0.75
FUZZY_MARGIN = 0.05
FUZZY_MIN_TOKENS = 8
#: A caller vote alone must not pair a function with a rewrite of it: below this the vote is refused.
CALLER_MIN_SIMILARITY = 0.5

#: Methods whose result is code-identical, written without ``--accept-fuzzy``.
TRUSTED_METHODS = ("exact", "neighbour")
REVIEW_METHODS = ("fuzzy", "caller")

GP = 28
_BRANCH_OPS = frozenset({0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17})
_REGIMM_BRANCH_RT = frozenset({0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13})
_ADDRESS_ARITHMETIC_OPS = frozenset({0x08, 0x09, 0x0D})
_FIRST_MEMORY_OP = 0x20


class BindingCarryError(ValueError):
    """Unusable input, named by a code."""

    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code


# ---------------------------------------------------------------------------
# One analysed module
# ---------------------------------------------------------------------------

class Module:
    """The callable functions of one analysed executable, with masked token streams."""

    def __init__(self, document: dict, imports: dict[int, str] | None = None):
        if document.get("schema") == analysis_ir.IR_SCHEMA:
            ir = document
        else:
            try:
                ir = analysis_ir.build_ir(document)
            except analysis_ir.IRError as exc:
                raise BindingCarryError("CARRY_INPUT", str(exc)) from exc
        self.imports = dict(imports or {})
        owned: dict[int, list[tuple[int, int]]] = collections.defaultdict(list)
        for row in ir["instructions"]:
            for owner in row["owners"]:
                owned[owner].append((row["address"], row["word"]))
        self.entries: list[int] = []
        self.function: dict[int, dict[str, Any]] = {}
        for fn in ir["functions"]:
            if fn["kind"] != "callable":
                continue
            entry = fn["entry"]
            rows = sorted(owned.get(entry, ()))
            if not rows or rows[0][0] != entry:
                continue
            calls = sorted(fn["calls"] + fn["tail_calls"], key=lambda item: item["site"])
            self.function[entry] = {
                "entry": entry,
                "addresses": [address for address, _ in rows],
                "words": [word for _, word in rows],
                "calls": [item["target"] for item in calls],
            }
        self.entries = sorted(self.function)
        self.owner_of: dict[int, list[int]] = collections.defaultdict(list)
        for entry, fn in self.function.items():
            for address in fn["addresses"]:
                self.owner_of[address].append(entry)
        for entry in self.entries:
            self.function[entry]["tokens"] = self._tokens(entry)
            self.function[entry]["digest"] = _digest(self.function[entry]["tokens"])

    def callee_class(self, entry: int, target: int | None) -> tuple:
        if target is None:
            return ("indirect",)
        if target == entry:
            return ("self",)
        if target in self.imports:
            return ("import", self.imports[target])
        return ("call",)

    def _tokens(self, entry: int) -> list[tuple]:
        fn = self.function[entry]
        end = fn["addresses"][-1] + 4
        stub = self.imports.get(entry)
        if stub is not None:
            return [("import-stub", stub)]
        high: set[int] = set()
        tokens: list[tuple] = []
        for address, word in zip(fn["addresses"], fn["words"], strict=True):
            op, rs, rt = word >> 26, (word >> 21) & 31, (word >> 16) & 31
            if op in (0x02, 0x03):
                target = ((address + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
                if op == 0x02 and entry <= target < end:
                    tokens.append(("j", target - entry))
                else:
                    tokens.append(("j" if op == 0x02 else "jal", self.callee_class(entry, target)))
            elif _is_branch(word):
                target = address + 4 + (_signed16(word) << 2)
                tokens.append(("b", word) if entry <= target < end else ("b-out", word & 0xFFFF0000))
            elif op == 0x0F:
                tokens.append(("lui", rt))
            elif (op in _ADDRESS_ARITHMETIC_OPS or op >= _FIRST_MEMORY_OP) and (rs in high or rs == GP):
                tokens.append(("addr", word >> 16))
            else:
                tokens.append(("w", word))
            effect = analysis_ir.gpr_effects(word)
            if op == 0x0F:
                high.add(rt)
            elif not effect["known"]:
                high.clear()
            else:
                high.difference_update(effect["writes"])
        return tokens


def _signed16(word: int) -> int:
    value = word & 0xFFFF
    return value - 0x10000 if value & 0x8000 else value


def _is_branch(word: int) -> bool:
    op, rs, rt = word >> 26, (word >> 21) & 31, (word >> 16) & 31
    return op in _BRANCH_OPS or (op == 0x01 and rt in _REGIMM_BRANCH_RT) or (op == 0x11 and rs == 0x08)


def _digest(tokens: list[tuple]) -> str:
    return hashlib.sha256(repr(tokens).encode("ascii")).hexdigest()


def load_module(cfg_path: Path, imports_path: Path | None) -> Module:
    document = _read_json(cfg_path)
    if not isinstance(document, dict):
        raise BindingCarryError("CARRY_INPUT", f"{cfg_path}: not a CFG report or analysis IR object")
    imports: dict[int, str] = {}
    if imports_path is not None:
        raw = _read_json(imports_path)
        if not isinstance(raw, dict):
            raise BindingCarryError("CARRY_INPUT", f"{imports_path}: expected an object of address -> NID")
        for key, value in raw.items():
            try:
                imports[int(key, 0)] = str(value).lower()
            except ValueError as exc:
                raise BindingCarryError("CARRY_INPUT", f"{imports_path}: bad address {key!r}") from exc
    return Module(document, imports)


def _read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BindingCarryError("CARRY_INPUT", f"{path}: {exc}") from exc


# ---------------------------------------------------------------------------
# Matching
# ---------------------------------------------------------------------------

def _ratio(a: list[tuple], b: list[tuple]) -> float:
    return difflib.SequenceMatcher(None, a, b, autojunk=False).ratio()


class Matcher:
    """Runs the ladder and keeps, per old function, its match, method and evidence."""

    def __init__(self, old: Module, new: Module, min_similarity: float, margin: float):
        self.old, self.new = old, new
        self.min_similarity, self.margin = min_similarity, margin
        self.match: dict[int, int] = {}
        self.method: dict[int, str] = {}
        self.similarity: dict[int, float] = {}
        self.refused: dict[int, dict[str, Any]] = {}
        self.taken: dict[int, int] = {}

    def _assign(self, old_entry: int, new_entry: int, method: str, similarity: float | None = None) -> None:
        self.match[old_entry] = new_entry
        self.taken[new_entry] = old_entry
        self.method[old_entry] = method
        if similarity is None:
            similarity = 1.0 if method in TRUSTED_METHODS else _ratio(
                self.old.function[old_entry]["tokens"], self.new.function[new_entry]["tokens"])
        self.similarity[old_entry] = similarity
        self.refused.pop(old_entry, None)

    def run(self) -> None:
        self._exact()
        self._neighbour()
        self._fuzzy()
        self._caller()

    # -- 1. exact -----------------------------------------------------------
    def _groups(self) -> dict[str, tuple[list[int], list[int]]]:
        groups: dict[str, tuple[list[int], list[int]]] = {}
        for entry in self.old.entries:
            if entry not in self.match:
                groups.setdefault(self.old.function[entry]["digest"], ([], []))[0].append(entry)
        for entry in self.new.entries:
            if entry not in self.taken:
                digest = self.new.function[entry]["digest"]
                if digest in groups:
                    groups[digest][1].append(entry)
        return {digest: pair for digest, pair in groups.items() if pair[0] and pair[1]}

    def _exact(self) -> None:
        for _digest_key, (olds, news) in sorted(self._groups().items()):
            if len(olds) == 1 and len(news) == 1:
                self._assign(olds[0], news[0], "exact")

    # -- 2. neighbour tie-break --------------------------------------------
    def _anchor(self, entries: list[int], entry: int, anchors: set[int], step: int) -> int | None:
        index = entries.index(entry) + step
        while 0 <= index < len(entries):
            if entries[index] in anchors:
                return entries[index]
            index += step
        return None

    def _context_score(self, old_entry: int, new_entry: int, old_anchors: set[int], new_anchors: set[int]) -> int:
        score = 0
        old_calls = self.old.function[old_entry]["calls"]
        new_calls = self.new.function[new_entry]["calls"]
        for old_target, new_target in zip(old_calls, new_calls, strict=False):
            if old_target in self.match:
                score += 1 if self.match[old_target] == new_target else -1
        for step in (-1, 1):
            old_near = self._anchor(self.old.entries, old_entry, old_anchors, step)
            new_near = self._anchor(self.new.entries, new_entry, new_anchors, step)
            if old_near is not None and new_near is not None and self.match[old_near] == new_near:
                score += 1
        return score

    def _neighbour(self) -> None:
        changed = True
        while changed:
            changed = False
            anchors_old = set(self.match)
            anchors_new = set(self.taken)
            picks: list[tuple[int, int]] = []
            for _digest_key, (olds, news) in sorted(self._groups().items()):
                scores = {(o, n): self._context_score(o, n, anchors_old, anchors_new) for o in olds for n in news}
                for old_entry in olds:
                    best = self._unique_best([scores[(old_entry, n)] for n in news])
                    if best is None or scores[(old_entry, news[best])] < 1:
                        continue
                    new_entry = news[best]
                    back = self._unique_best([scores[(o, new_entry)] for o in olds])
                    if back is not None and olds[back] == old_entry:
                        picks.append((old_entry, new_entry))
                if len(olds) > 1 or len(news) > 1:
                    for old_entry in olds:
                        self.refused[old_entry] = {"reason": "several functions share this code and their "
                                                             "neighbours do not tell them apart",
                                                   "candidates": sorted(news), "exact_code": True}
            for old_entry, new_entry in picks:
                self._assign(old_entry, new_entry, "neighbour")
                changed = True

    @staticmethod
    def _unique_best(values: list[int]) -> int | None:
        best = max(values)
        return values.index(best) if values.count(best) == 1 else None

    # -- 3. fuzzy -----------------------------------------------------------
    def _fuzzy(self) -> None:
        free_new = [e for e in self.new.entries if e not in self.taken]
        ranking: dict[int, list[tuple[float, int]]] = {}
        for old_entry in self.old.entries:
            if old_entry in self.match or self.refused.get(old_entry, {}).get("exact_code"):
                continue
            old_tokens = self.old.function[old_entry]["tokens"]
            scored: list[tuple[float, int]] = []
            floor = 0.0
            for new_entry in free_new:
                new_tokens = self.new.function[new_entry]["tokens"]
                matcher = difflib.SequenceMatcher(None, old_tokens, new_tokens, autojunk=False)
                if len(scored) >= 2 and matcher.real_quick_ratio() <= floor:
                    continue
                if len(scored) >= 2 and matcher.quick_ratio() <= floor:
                    continue
                scored.append((matcher.ratio(), new_entry))
                scored.sort(key=lambda item: (-item[0], item[1]))
                scored = scored[:3]
                if len(scored) >= 2:
                    floor = scored[1][0]
            ranking[old_entry] = scored
        claims: dict[int, list[int]] = collections.defaultdict(list)
        for old_entry, scored in ranking.items():
            if not scored:
                continue
            best_ratio, best_entry = scored[0]
            runner = scored[1][0] if len(scored) > 1 else 0.0
            tokens = len(self.old.function[old_entry]["tokens"])
            if best_ratio >= self.min_similarity and tokens >= FUZZY_MIN_TOKENS:
                if best_ratio - runner >= self.margin:
                    claims[best_entry].append(old_entry)
                else:
                    self.refused[old_entry] = {
                        "reason": f"fuzzy: runner-up within {self.margin:.2f} of the best",
                        "candidates": [entry for _, entry in scored[:2]],
                        "similarity": round(best_ratio, 4)}
            else:
                self.refused[old_entry] = {
                    "reason": "no new function is similar enough" if tokens >= FUZZY_MIN_TOKENS
                    else "too short to compare by similarity",
                    "closest": best_entry, "similarity": round(best_ratio, 4)}
        for new_entry, olds in sorted(claims.items()):
            if len(olds) == 1:
                self._assign(olds[0], new_entry, "fuzzy")
            else:
                for old_entry in olds:
                    self.refused[old_entry] = {"reason": "several old functions claim the same new function",
                                               "candidates": [new_entry]}

    # -- 4. via callers -----------------------------------------------------
    def _caller(self) -> None:
        trusted = {o for o, m in self.method.items() if m in TRUSTED_METHODS}
        changed = True
        while changed:
            changed = False
            votes: dict[int, set[int]] = collections.defaultdict(set)
            for caller in sorted(trusted):
                old_calls = self.old.function[caller]["calls"]
                new_calls = self.new.function[self.match[caller]]["calls"]
                if len(old_calls) != len(new_calls):
                    continue
                for old_target, new_target in zip(old_calls, new_calls, strict=False):
                    if old_target in self.old.function and old_target not in self.match \
                            and not self.refused.get(old_target, {}).get("exact_code") \
                            and new_target in self.new.function and new_target not in self.taken:
                        votes[old_target].add(new_target)
            used: collections.Counter = collections.Counter()
            for candidates in votes.values():
                if len(candidates) == 1:
                    used[next(iter(candidates))] += 1
            for old_entry, candidates in sorted(votes.items()):
                if len(candidates) == 1 and used[next(iter(candidates))] == 1:
                    new_entry = next(iter(candidates))
                    similarity = _ratio(self.old.function[old_entry]["tokens"], self.new.function[new_entry]["tokens"])
                    if similarity < CALLER_MIN_SIMILARITY:
                        self.refused[old_entry] = {
                            "reason": "its callers point at a function that shares too little code with it",
                            "closest": new_entry, "similarity": round(similarity, 4)}
                        continue
                    self._assign(old_entry, new_entry, "caller", similarity)
                    changed = True
                else:
                    self.refused[old_entry] = {"reason": "callers disagree about which new function it is",
                                               "candidates": sorted(candidates)}

    # -- queries --------------------------------------------------------------
    def status(self, old_entry: int) -> dict[str, Any]:
        if old_entry in self.match:
            return {"status": self.method[old_entry], "new": self.match[old_entry],
                    "similarity": round(self.similarity[old_entry], 4)}
        info = dict(self.refused.get(old_entry, {"reason": "no candidate"}))
        info["status"] = "ambiguous" if "candidates" in info and "closest" not in info else "unmatched"
        return info

    def align(self, old_entry: int, old_address: int) -> int | None:
        """The new address of ``old_address`` inside the matched function, or ``None``."""
        new_entry = self.match[old_entry]
        old_fn, new_fn = self.old.function[old_entry], self.new.function[new_entry]
        index = old_fn["addresses"].index(old_address)
        if index == 0:
            return new_entry
        matcher = difflib.SequenceMatcher(None, old_fn["tokens"], new_fn["tokens"], autojunk=False)
        for a, b, size in matcher.get_matching_blocks():
            if a <= index < a + size:
                address: int = new_fn["addresses"][b + index - a]
                if self.method[old_entry] in TRUSTED_METHODS and address - new_entry != old_address - old_entry:
                    return None
                return address
        return None


# ---------------------------------------------------------------------------
# Bindings
# ---------------------------------------------------------------------------

def binding_sites(bindings: dict[str, Any]) -> list[dict[str, Any]]:
    """Every address in a validated ``runtime_bindings`` block, in a stable order."""
    sites = [{"name": name, "address": bindings[name]}
             for name in title_manifest.RUNTIME_BINDING_FIELDS if name in bindings]
    for index, alias in enumerate(bindings.get("dispatch_aliases", ())):
        sites.append({"name": f"dispatch_aliases[{index}].from", "address": alias["from"]})
        sites.append({"name": f"dispatch_aliases[{index}].to", "address": alias["to"]})
    return sites


def carry_sites(matcher: Matcher, sites: list[dict[str, Any]]) -> list[dict[str, Any]]:
    results = []
    for site in sites:
        row = dict(site)
        owners = [e for e in matcher.old.owner_of.get(site["address"], ()) if e in matcher.old.function]
        if len(owners) != 1:
            row.update(status="unmatched",
                       reason="the address is in no analysed function" if not owners
                       else "the address is shared by several functions")
        else:
            row["function"] = owners[0]
            row.update(matcher.status(owners[0]))
            if row["status"] in TRUSTED_METHODS + REVIEW_METHODS:
                address = matcher.align(owners[0], site["address"])
                if address is None:
                    row.update(status="unmatched", reason="the address is inside the function but its code "
                                                          "does not line up with the new function",
                               matched_function=row.pop("new"))
                else:
                    row["new"] = address
        results.append(row)
    return results


def propose_block(bindings: dict[str, Any], rows: list[dict[str, Any]], accept_fuzzy: bool) -> tuple[dict, list[str]]:
    """The new ``runtime_bindings`` block built from the carried rows, and the notes on what was left out."""
    allowed = TRUSTED_METHODS + (REVIEW_METHODS if accept_fuzzy else ())
    carried = {row["name"]: row["new"] for row in rows if row["status"] in allowed and "new" in row}
    notes: list[str] = []
    block: dict[str, Any] = {"schema_version": bindings["schema_version"]}
    for name in title_manifest.RUNTIME_BINDING_FIELDS:
        if name in carried:
            block[name] = carried[name]
        elif name in bindings:
            notes.append(f"{name}: not carried")
    aliases = []
    for index in range(len(bindings.get("dispatch_aliases", ()))):
        pair = (carried.get(f"dispatch_aliases[{index}].from"), carried.get(f"dispatch_aliases[{index}].to"))
        if None in pair:
            notes.append(f"dispatch_aliases[{index}]: dropped, both ends are needed")
        else:
            aliases.append({"from": pair[0], "to": pair[1]})
    if aliases:
        block["dispatch_aliases"] = aliases
    for name in title_manifest.RUNTIME_BINDING_COUNTS:
        if name in bindings:
            notes.append(f"{name}: not an address, so not carried; set it for the new revision by hand")
    return block, notes


def run(args: argparse.Namespace) -> tuple[int, dict[str, Any]]:
    try:
        manifest = title_manifest.validate_manifest(title_manifest.load_manifest(Path(args.manifest)))
    except title_manifest.TitleManifestError as exc:
        raise BindingCarryError("CARRY_MANIFEST", str(exc)) from exc
    bindings = manifest.get("runtime_bindings")
    if not bindings:
        raise BindingCarryError("CARRY_MANIFEST", "the manifest has no runtime_bindings to carry")
    old = load_module(Path(args.old_cfg), Path(args.old_imports) if args.old_imports else None)
    new = load_module(Path(args.new_cfg), Path(args.new_imports) if args.new_imports else None)
    matcher = Matcher(old, new, args.min_similarity, args.margin)
    matcher.run()
    rows = carry_sites(matcher, binding_sites(bindings))
    block, notes = propose_block(bindings, rows, args.accept_fuzzy)
    proposal = None
    if len(block) > 1:
        try:
            proposal = title_manifest.validate_runtime_bindings(block, "$.runtime_bindings")
        except title_manifest.TitleManifestError as exc:
            raise BindingCarryError("CARRY_PROPOSAL_INVALID", f"the carried block is not valid: {exc}") from exc
    complete = all(row["status"] in TRUSTED_METHODS + (REVIEW_METHODS if args.accept_fuzzy else ())
                   for row in rows) and not any("not carried" in n or "dropped" in n for n in notes)
    counts = collections.Counter(matcher.status(entry)["status"] for entry in old.entries)
    report = {
        "schema": "nakagawa-binding-carry-report-v1",
        "private": True,
        "accept_fuzzy": bool(args.accept_fuzzy),
        "functions": {"old": len(old.entries), "new": len(new.entries), "by_status": dict(sorted(counts.items()))},
        "bindings": rows,
        "notes": notes,
        "proposed_runtime_bindings": proposal,
    }
    return (0 if complete else 1), report


def format_report(report: dict[str, Any]) -> str:
    lines = ["# PRIVATE: derived from a title's analysis; do not publish (AGENTS.md section 5)"]
    for row in report["bindings"]:
        new = f"0x{row['new']:08x}" if "new" in row else "-"
        extra = ""
        if row["status"] in REVIEW_METHODS:
            extra = f"  similarity {row['similarity']:.2f}  CHECK BY HAND"
        elif row["status"] == "neighbour":
            extra = "  identical code, told apart by its neighbours"
        elif row["status"] in ("ambiguous", "unmatched"):
            parts = [row.get("reason", "")]
            if "closest" in row:
                parts.append(f"closest 0x{row['closest']:08x} at {row['similarity']:.2f}")
            if "candidates" in row:
                parts.append("candidates " + ", ".join(f"0x{c:08x}" for c in row["candidates"]))
            extra = "  " + "; ".join(p for p in parts if p)
        lines.append(f"{row['status'].upper():<9} 0x{row['address']:08x} -> {new}  {row['name']}{extra}")
    lines.extend(f"NOTE      {note}" for note in report["notes"])
    functions = report["functions"]
    summary = ", ".join(f"{count} {name}" for name, count in functions["by_status"].items())
    lines.append(f"functions: {functions['old']} old, {functions['new']} new; old side: {summary}")
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--old-cfg", required=True, help="CFG report or analysis IR of the revision the manifest describes")
    parser.add_argument("--new-cfg", required=True, help="CFG report or analysis IR of the other revision")
    parser.add_argument("--manifest", required=True, help="title manifest whose runtime_bindings to carry (read only)")
    parser.add_argument("--old-imports", help="JSON address -> NID for the old module's import stubs")
    parser.add_argument("--new-imports", help="JSON address -> NID for the new module's import stubs")
    parser.add_argument("--accept-fuzzy", action="store_true",
                        help="also write fuzzy and caller matches (each is still flagged for review)")
    parser.add_argument("--min-similarity", type=float, default=FUZZY_MIN_SIMILARITY)
    parser.add_argument("--margin", type=float, default=FUZZY_MARGIN)
    parser.add_argument("--out-bindings", help="write the proposed runtime_bindings JSON here instead of stdout")
    parser.add_argument("--report", help="write the JSON report here")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        code, report = run(args)
    except BindingCarryError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    print(format_report(report), file=sys.stderr)
    if args.report:
        Path(args.report).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    proposal = report["proposed_runtime_bindings"]
    if proposal is None:
        print("CARRY_NOTHING: no binding could be carried; nothing proposed", file=sys.stderr)
        return 1
    text = json.dumps(proposal, indent=2) + "\n"
    if args.out_bindings:
        Path(args.out_bindings).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    if code:
        print("CARRY_INCOMPLETE: some bindings were not carried; see the lines above", file=sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main())
