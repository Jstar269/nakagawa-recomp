#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Regenerate and check the CPU runtime helper inventory (issue #346).

The committed inventory records only the facts that the #346 planning depends on,
one row per helper of ``src/rt/recomp.c`` and ``src/rt/recomp.h``:

  helper      the function name
  defined in  the defining production ``src/`` file(s); ``none`` when only a header declares it
  recomp.h    whether ``recomp.h`` declares or defines it
  reach       one of the reach classes below
  provenance  the public-ledger classification of each defining file

Reach classes, first match wins:

  emitter-reached          called by the generated-code emitter ``tools/codegen.py``, directly
                           or through a ``recomp.h`` macro whose body calls the helper (the
                           macro is named in the row)
  production-runtime-only  called from production ``src/`` code and not from the emitter
  selftest-only            called only from selftest harnesses (``*selftest*`` files)
  address-only             never called, but named as a value somewhere in ``src/``
  none                     no reference in ``src/`` or in the emitter

Line numbers and per-site call counts are deliberately not committed: they move with
unrelated edits. ``--details`` prints them to stdout and never writes them. ``--check``
fails only when a row changes: a helper is added or removed, its defining files or its
header declaration change, its reach class or emitter route changes, or the provenance
class of its defining file changes.

Usage:
    python tools/recomp_helper_inventory.py            # rewrite the committed inventory
    python tools/recomp_helper_inventory.py --check    # fail if it is stale
    python tools/recomp_helper_inventory.py --details  # print lines and call counts to stdout
"""

from __future__ import annotations

import argparse
import ast
import bisect
import json
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RECOMP_C = "src/rt/recomp.c"
RECOMP_H = "src/rt/recomp.h"
EMITTER = "tools/codegen.py"
LEDGER = "assets/public_provenance_ledger.json"
OUT_MD = ROOT / "docs" / "RECOMP_HELPER_INVENTORY.md"
REACH_CLASSES = ("emitter-reached", "production-runtime-only", "selftest-only", "address-only", "none")
DECLARED_ONLY = "declared-only"
NOT_IN_LEDGER = "not-in-public-ledger"
TOP_FILES = 6

_SPECIAL_RE = re.compile(r"[/#\"']")
_DELIM_RE = re.compile(r"[;{}]")
_BRACE_RE = re.compile(r"[{}]")
# Call sites in emitter text and macro bodies: an identifier followed by "(".
_CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
# A declarator never has "(*" right after its name: that is a function-pointer declarator.
_DECLARATOR_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\((?!\s*\*)")
# C source references: a bare identifier (group 1), or a call when group 2 matches.
# Member accesses (a.b, p->b) name a field, not a file-scope function.
_REF_RE = re.compile(r"(?<!->)(?<!\.)\b([A-Za-z_]\w*)\b(\s*\()?")
_IDENT_RE = re.compile(r"[A-Za-z_]\w*")
_NOT_A_HELPER = frozenset(
    {
        "if", "for", "while", "switch", "return", "sizeof", "do", "else",
        "_Static_assert", "static_assert", "_Alignas", "_Alignof", "__alignof__",
        "__attribute__", "__declspec", "__asm__", "__typeof__",
    }
)


def strip_c(text: str) -> str:
    """Blank comments, string and char literal bodies and preprocessor directives.

    Newlines are kept and the length is unchanged, so an offset in the result is
    an offset in ``text`` and maps to the same line.
    """
    out = list(text)
    n = len(text)
    i = 0
    while True:
        m = _SPECIAL_RE.search(text, i)
        if m is None:
            break
        i = m.start()
        c = text[i]
        if c == "/" and text.startswith("//", i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            _blank(out, i, end)
            i = end
        elif c == "/" and text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            _blank(out, i, end)
            i = end
        elif c == "#" and not text[text.rfind("\n", 0, i) + 1 : i].strip():
            end = i
            while end < n and text[end] != "\n":
                end += 2 if (text[end] == "\\" and text.startswith("\n", end + 1)) else 1
            _blank(out, i, min(end, n))
            i = end
        elif c in "\"'":
            end = i + 1
            while end < n and text[end] != c and text[end] != "\n":
                end += 2 if text[end] == "\\" else 1
            end = min(end, n)
            _blank(out, i + 1, end)
            i = end + 1 if end < n and text[end] == c else end
        else:
            i += 1
    return "".join(out)


def _blank(out: list[str], start: int, end: int) -> None:
    for k in range(start, end):
        if out[k] != "\n":
            out[k] = " "


@dataclass(frozen=True)
class Decl:
    name: str
    is_def: bool
    static: bool
    inline: bool
    name_offset: int
    end_offset: int  # closing brace for a definition, the ';' for a prototype


def _first_declarator(text: str) -> re.Match[str] | None:
    for m in _DECLARATOR_RE.finditer(text):
        if m.group(1) not in _NOT_A_HELPER:
            return m
    return None


def _declarator_head(chunk: str) -> re.Match[str] | None:
    """Return the declared-name match when ``chunk`` is a function head, else None."""
    body = chunk.rstrip()
    if not body.endswith(")") or any(t in body for t in ("=", "{", "}")):
        return None
    if re.match(r"\s*typedef\b", body):
        return None
    m = _first_declarator(body)
    # A declaration needs a return type before the name; a bare NAME(args); is a macro call.
    if m is None or not _IDENT_RE.search(body[: m.start(1)]):
        return None
    return m


def _make_decl(base: int, m: re.Match[str], is_def: bool, end: int, chunk: str) -> Decl:
    prefix = chunk[: m.start(1)]
    return Decl(
        name=m.group(1),
        is_def=is_def,
        static=re.search(r"\bstatic\b", prefix) is not None,
        inline=re.search(r"\b(?:__)?inline(?:__)?\b", prefix) is not None,
        name_offset=base + m.start(1),
        end_offset=end,
    )


def _matching_brace(stripped: str, open_pos: int) -> int:
    depth = 0
    for m in _BRACE_RE.finditer(stripped, open_pos):
        depth += 1 if m.group() == "{" else -1
        if depth == 0:
            return m.start()
    return len(stripped) - 1


def top_level_decls(stripped: str) -> list[Decl]:
    """Function definitions and function prototypes at file scope.

    ``stripped`` must come from :func:`strip_c`. Braces and semicolons nested in a
    struct, union or initializer belong to the enclosing statement and are skipped.
    """
    decls: list[Decl] = []
    depth = 0
    stmt = 0
    pos = 0
    while True:
        m = _DELIM_RE.search(stripped, pos)
        if m is None:
            break
        p = m.start()
        ch = stripped[p]
        if ch == "{":
            if depth == 0:
                head = stripped[stmt:p]
                hm = _declarator_head(head)
                if hm is not None:
                    close = _matching_brace(stripped, p)
                    decls.append(_make_decl(stmt, hm, True, close, head))
                    stmt = close + 1
                    pos = close + 1
                    continue
            depth += 1
        elif ch == "}":
            if depth > 0:
                depth -= 1
        elif depth == 0:  # ';' at file scope
            chunk = stripped[stmt:p]
            pm = _declarator_head(chunk)
            if pm is not None:
                decls.append(_make_decl(stmt, pm, False, p, chunk))
            stmt = p + 1
        pos = p + 1
    return decls


def _line_starts(text: str) -> list[int]:
    return [0] + [m.end() for m in re.finditer(r"\n", text)]


def _line(starts: list[int], offset: int) -> int:
    return bisect.bisect_right(starts, offset)


def macro_calls(raw: str) -> dict[str, set[str]]:
    """Map each ``#define`` name to the call-like identifiers in its body."""
    joined = raw.replace("\\\n", " ")
    calls: dict[str, set[str]] = {}
    for line in joined.split("\n"):
        m = re.match(r"\s*#\s*define\s+([A-Za-z_]\w*)", line)
        if m is None:
            continue
        body = strip_c(line[m.end():])
        calls.setdefault(m.group(1), set()).update(
            c.group(1) for c in _CALL_RE.finditer(body) if c.group(1) not in _NOT_A_HELPER
        )
    return calls


def emitter_text(source: str) -> str:
    """The string-literal text of a Python emitter: the C it writes, minus docstrings."""
    tree = ast.parse(source)
    docstrings: set[int] = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)) and node.body:
            first = node.body[0]
            if isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant):
                docstrings.add(id(first.value))
    return "\n".join(
        node.value
        for node in ast.walk(tree)
        if isinstance(node, ast.Constant) and isinstance(node.value, str) and id(node) not in docstrings
    )


@dataclass
class Helper:
    name: str
    header_line: int | None = None
    defs: list[tuple[str, int, int]] = field(default_factory=list)  # file, first line, last line


def _short(rel: str) -> str:
    return rel.removeprefix("src/")


def _is_harness(rel: str) -> bool:
    """Selftest sources define stubs so that a harness links; they are not the runtime."""
    return "selftest" in Path(rel).name


def _home(defined: list[str]) -> str | None:
    if not defined:
        return None
    for preferred in (RECOMP_C, RECOMP_H):
        if preferred in defined:
            return preferred
    return defined[0]


def _reach(direct: int, macros: dict[str, int], prod: Counter, harn: Counter, addr: Counter) -> str:
    if direct or macros:
        return "emitter-reached"
    if sum(prod.values()):
        return "production-runtime-only"
    if sum(harn.values()):
        return "selftest-only"
    if sum(addr.values()):
        return "address-only"
    return "none"


def load_inputs(root: Path = ROOT, overrides: dict[str, str] | None = None) -> tuple[dict[str, str], str, dict]:
    """Read the C sources under ``src/``, the emitter and the public ledger.

    ``overrides`` replaces the text of a repository-relative path (tests mutate an
    input in memory this way without touching the tree).
    """
    overrides = overrides or {}

    def read(rel: str) -> str:
        if rel in overrides:
            return overrides[rel]
        return (root / rel).read_text(encoding="utf-8", errors="replace")

    sources: dict[str, str] = {}
    for p in sorted((root / "src").rglob("*")):
        if p.is_file() and p.suffix in (".c", ".h"):
            rel = p.relative_to(root).as_posix()
            sources[rel] = read(rel)
    for rel in overrides:
        if rel.startswith("src/") and rel.endswith((".c", ".h")):
            sources[rel] = overrides[rel]
    return sources, read(EMITTER), json.loads(read(LEDGER))


def build_inventory(sources: dict[str, str], emitter: str, ledger: dict) -> dict:
    """Pure inventory from in-memory inputs.

    Each row carries the committed facts. Line numbers and call counts live under
    ``details`` and are only printed by ``--details``.
    """
    if RECOMP_C not in sources or RECOMP_H not in sources:
        raise ValueError(f"missing {RECOMP_C} or {RECOMP_H}")
    parsed: dict[str, tuple[str, list[int], list[Decl]]] = {}
    for rel in sorted(sources):
        text = sources[rel]
        stripped = strip_c(text)
        parsed[rel] = (stripped, _line_starts(text), top_level_decls(stripped))

    own = (RECOMP_C, RECOMP_H)
    helpers: dict[str, Helper] = {}
    for rel in own:
        _stripped, starts, decls = parsed[rel]
        for d in decls:
            h = helpers.setdefault(d.name, Helper(d.name))
            first = _line(starts, d.name_offset)
            if rel == RECOMP_H and h.header_line is None:
                h.header_line = first
            if d.is_def:
                h.defs.append((rel, first, _line(starts, d.end_offset)))

    for rel in sorted(parsed):
        if rel in own or _is_harness(rel):
            continue
        _stripped, starts, decls = parsed[rel]
        for d in decls:
            if d.is_def and not d.static and d.name in helpers:
                first = _line(starts, d.name_offset)
                helpers[d.name].defs.append((rel, first, _line(starts, d.end_offset)))

    decl_positions = {
        (rel, d.name_offset)
        for rel, (_stripped, _starts, decls) in parsed.items()
        for d in decls
        if d.name in helpers
    }

    macro_map: dict[str, list[str]] = {}
    for macro, called in macro_calls(sources[RECOMP_H]).items():
        hits = sorted(c for c in called if c in helpers and c != macro)
        if hits and macro not in helpers:
            macro_map[macro] = hits

    emitted = Counter(m.group(1) for m in _CALL_RE.finditer(emitter_text(emitter)))

    runtime: dict[str, Counter] = defaultdict(Counter)
    harness: dict[str, Counter] = defaultdict(Counter)
    address: dict[str, Counter] = defaultdict(Counter)
    for rel in sorted(parsed):
        stripped = parsed[rel][0]
        bucket = harness if _is_harness(rel) else runtime
        for m in _REF_RE.finditer(stripped):
            name = m.group(1)
            if name not in helpers and name not in macro_map:
                continue
            if (rel, m.start(1)) in decl_positions:
                continue
            if m.group(2) is not None:
                if name in helpers:
                    bucket[name][rel] += 1
                for h in macro_map.get(name, ()):
                    bucket[h][rel] += 1
            elif name in helpers:
                address[name][rel] += 1

    entries = {e["path"]: e for e in ledger.get("entries", [])}

    def classification(rel: str) -> str:
        entry = entries.get(rel)
        if entry is None:
            return NOT_IN_LEDGER
        return str(entry.get("classification", "unclassified"))

    rows = []
    for name in sorted(helpers):
        h = helpers[name]
        defined = sorted({f for f, _a, _b in h.defs})
        direct = emitted.get(name, 0)
        macros = {m: emitted[m] for m, hs in macro_map.items() if name in hs and emitted.get(m, 0)}
        prod = runtime.get(name, Counter())
        harn = harness.get(name, Counter())
        addr = address.get(name, Counter())
        home = _home(defined)
        rows.append(
            {
                "name": name,
                "defined_in": defined,
                "in_header": h.header_line is not None,
                "reach": _reach(direct, macros, prod, harn, addr),
                "emitter_direct": direct > 0,
                "emitter_macros": sorted(macros),
                "provenance": [(f, classification(f)) for f in defined],
                "home_class": classification(home) if home else DECLARED_ONLY,
                "details": {
                    "defs": [{"file": f, "first": a, "last": b} for f, a, b in h.defs],
                    "header_line": h.header_line,
                    "emitter_direct": direct,
                    "emitter_macros": macros,
                    "runtime": dict(sorted(prod.items())),
                    "harness": dict(sorted(harn.items())),
                    "address": dict(sorted(addr.items())),
                },
            }
        )
    return {
        "schema": 2,
        "sources": [RECOMP_C, RECOMP_H, EMITTER, LEDGER],
        "summary": _summarize(rows),
        "helpers": rows,
    }


def _summarize(rows: list[dict]) -> dict:
    reach = Counter(r["reach"] for r in rows)
    home = Counter(r["home_class"] for r in rows)
    return {
        "total": len(rows),
        "in_header": sum(1 for r in rows if r["in_header"]),
        "reach": {c: reach.get(c, 0) for c in REACH_CLASSES},
        "provenance": dict(sorted(home.items())),
    }


def _defined_cell(row: dict) -> str:
    return ", ".join(f"`{_short(f)}`" for f in row["defined_in"]) or "none"


def _reach_cell(row: dict) -> str:
    if row["reach"] != "emitter-reached":
        return row["reach"]
    routes = (["direct"] if row["emitter_direct"] else []) + [f"via `{m}`" for m in row["emitter_macros"]]
    return f"emitter-reached ({', '.join(routes)})"


def _provenance_cell(row: dict) -> str:
    if not row["provenance"]:
        return "declared-only (no definition under `src/`)"
    return "; ".join(f"`{_short(f)}`: {c}" for f, c in row["provenance"])


def render_markdown(inv: dict) -> str:
    """The committed inventory: stable facts only, no line numbers and no call counts."""
    s = inv["summary"]
    reach_line = ", ".join(f"{c} {s['reach'][c]}" for c in REACH_CLASSES)
    prov_line = ", ".join(f"{c} {n}" for c, n in s["provenance"].items()) or "none"
    lines = [
        "# CPU runtime helper inventory",
        "",
        "<!-- Generated by tools/recomp_helper_inventory.py. Do not hand-edit. -->",
        "",
        "<!-- markdownlint-disable MD013 -->",
        "",
        "Tracking issue #346 (independence: replace inherited CPU runtime semantics). One row per",
        "helper of `src/rt/recomp.c` and `src/rt/recomp.h`: where it is defined, whether `recomp.h`",
        "declares it, how generated code and the runtime reach it, and the provenance class of its",
        "defining file. It is a text inventory, not a call graph, a semantic review or a provenance",
        "verdict. Only names, file names and classes appear here; no upstream source is read or copied.",
        "",
        "Regenerate with `python tools/recomp_helper_inventory.py`. `--check` fails only when a helper",
        "is added or removed, its defining files or header declaration change, its reach class or",
        "emitter route changes, or the provenance class of its defining file changes. Line numbers and",
        "call counts are not recorded here, so a line shift or an extra call site in an unrelated change",
        "does not make this file stale. `--details` prints them to stdout.",
        "",
        "## Method",
        "",
        "- **Helpers**: every file-scope function definition in `recomp.c`; every static inline",
        "  definition and every prototype in `recomp.h`. A non-static definition in another production",
        "  `src/` file is a defining file. Selftest harness definitions are not.",
        "- **Reach**, first match wins: `emitter-reached` (named in the string literals of",
        "  `tools/codegen.py`, the C it emits; directly, or through the `recomp.h` macros named in the",
        "  row); `production-runtime-only` (called from production `src/` code); `selftest-only` (called",
        "  only from `*selftest*` harness files); `address-only` (never called, but named as a value in",
        "  `src/`); `none`.",
        "- **Provenance**: the classification in `assets/public_provenance_ledger.json` of each defining",
        "  file. The summary counts the home definition: `recomp.c`, then `recomp.h`, then the first other",
        "  defining file.",
        "- **Not covered**: a call through a function pointer counts as an address reference, not a call.",
        "  `#if` arms are not evaluated (both count). Comments, literals and member accesses are ignored.",
        "  C harnesses written by other tools (`tools/codegen_gate.py`, `tools/vfpu_coverage_report.py`)",
        "  are not emitter callers.",
        "",
        "## Summary",
        "",
        f"- helpers: **{s['total']}** (declared in `recomp.h`: {s['in_header']})",
        f"- reach class: {reach_line}",
        f"- provenance class of the home definition: {prov_line}",
        "",
        "## Helpers",
        "",
        "| helper | defined in | recomp.h | reach | provenance |",
        "| --- | --- | --- | --- | --- |",
    ]
    for r in inv["helpers"]:
        in_header = "yes" if r["in_header"] else "no"
        lines.append(
            f"| `{r['name']}` | {_defined_cell(r)} | {in_header} | {_reach_cell(r)} | {_provenance_cell(r)} |"
        )
    lines.append("")
    return "\n".join(lines)


def _sites(counts: dict[str, int]) -> str:
    total = sum(counts.values())
    if not total:
        return "0"
    top = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))
    shown = ", ".join(f"`{_short(f)}` {n}" for f, n in top[:TOP_FILES])
    if len(top) > TOP_FILES:
        shown += f", +{len(top) - TOP_FILES} files"
    return f"{total}: {shown}"


def render_details(inv: dict) -> str:
    """Line numbers and call counts for review. Printed to stdout, never committed."""
    lines = [
        "| helper | defined at (lines) | recomp.h line | emitter direct | emitter via macro | production sites | selftest sites | address refs |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for r in inv["helpers"]:
        d = r["details"]
        spans = []
        for x in d["defs"]:
            span = f"L{x['first']}" if x["first"] == x["last"] else f"L{x['first']}-{x['last']}"
            spans.append(f"`{_short(x['file'])}` {span}")
        defined = "; ".join(spans) or "none"
        header = f"L{d['header_line']}" if d["header_line"] is not None else "-"
        macros = ", ".join(f"`{m}` {n}" for m, n in sorted(d["emitter_macros"].items())) or "-"
        lines.append(
            f"| `{r['name']}` | {defined} | {header} | {d['emitter_direct']} | {macros} | "
            f"{_sites(d['runtime'])} | {_sites(d['harness'])} | {_sites(d['address'])} |"
        )
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument(
        "--check",
        action="store_true",
        help="exit 1 if docs/RECOMP_HELPER_INVENTORY.md differs from a fresh generation",
    )
    ap.add_argument(
        "--details",
        action="store_true",
        help="print line numbers and call counts to stdout; writes and checks nothing",
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=OUT_MD,
        help="output path (default: docs/RECOMP_HELPER_INVENTORY.md)",
    )
    args = ap.parse_args(argv)
    if args.details and args.check:
        ap.error("--details prints only; it cannot be combined with --check")

    try:
        inv = build_inventory(*load_inputs())
    except (OSError, ValueError, SyntaxError, KeyError) as e:  # fail closed on unreadable inputs
        print(f"recomp_helper_inventory: {e}", file=sys.stderr)
        return 2

    if args.details:
        sys.stdout.write(render_details(inv))
        return 0

    text = render_markdown(inv)
    if args.check:
        if not args.out.is_file():
            print(f"recomp_helper_inventory: missing {args.out}; run python tools/regen_all.py", file=sys.stderr)
            return 1
        if args.out.read_text(encoding="utf-8") != text:
            print(
                f"recomp_helper_inventory: {args.out} is stale; run python tools/regen_all.py",
                file=sys.stderr,
            )
            return 1
        print(f"recomp_helper_inventory: {args.out} is current")
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8", newline="\n")
    print(f"recomp_helper_inventory: wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
