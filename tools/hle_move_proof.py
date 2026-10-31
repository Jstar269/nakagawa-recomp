#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Prove that a change to the runtime HLE sources only moved code (issue #307).

Compares the runtime HLE sources at a base revision with the working tree's
(``tools/hle_sources.py``) and fails unless the change is a move:

L1, items. Every top-level function, object, type, macro and non-conditional directive of
    the base appears in the new sources exactly once (macros at least once), byte for byte,
    under the same ``#if`` conditions. The one allowed edit to a moved item is dropping a
    leading ``static``, and only for a name another HLE file now uses. Registration
    functions (those that call ``sr_hle_register*`` or another registration function) may
    be regrouped; L3 proves them. A function with no base counterpart may hold registration
    calls and calls to other registration functions, nothing else. The multiset of string
    literals is unchanged. Base comments missing from the new sources are listed for review.
L2, preprocessed functions. For each build configuration, ``cc -E`` of every unit; every
    base function and object has the same token sequence after preprocessing. This is what
    catches a macro that expands differently in its new file, or a function compiled under
    a different condition.
L3, init sequence. In each configuration ``sr_hle_init`` is flattened by inlining every
    no-argument call to a registration function; the token streams must be equal, so the
    registration order and the side effects of initialisation are unchanged.
L4, artefacts (CLI only). The HLE manifest and the production/selftest reachability of
    every NID are unchanged apart from ``owner_file``, and ``tools/import_audit_baseline.json``
    is byte-identical to the base's.

CLI: ``python tools/hle_move_proof.py --base origin/main`` exits 0 when every layer passes.
``--map`` writes the function -> new file map other branches use to port their changes.
"""

from __future__ import annotations

import argparse
import collections
from dataclasses import dataclass, field
import io
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hle_sources  # noqa: E402
from hle_sources import Item, blank_comments_and_literals, is_object_definition, split_items  # noqa: E402

ROOT = hle_sources.ROOT

Unit = tuple[str, str]

INCLUDE_DIRS = ("src/rt", "src/core", "src/rt/atrac3p", "src/rt/atrac3p/libavcodec", "src/rt/atrac3p/libavutil")
#: The Makefile's CFLAGS defines, with the two its ``override`` lines always add.
_MAKE = ("SR_SDL3VK", "_CRT_SECURE_NO_WARNINGS", "SR_PUBLIC_SAFE", "SR_FLIGHT_RECORDER_LINKED",
         "PERF_AOT_INSTRUCTIONS=0")
_SELFTEST = _MAKE + ("SR_HLE_THREAD_SELFTEST", "SR_CORO_LIFECYCLE_TEST", "SR_SCHED_LIVENESS_TEST")
#: The configurations the HLE sources are compiled in: name -> (defines, extra compiler flags).
#: At the go these are checked against ``make -n -B`` of every recipe that compiles HLE.
CONFIGS: dict[str, tuple[tuple[str, ...], tuple[str, ...]]] = {
    "production": (_MAKE, ()),
    "thread-selftest": (_SELFTEST, ()),
    "aot-boundary-selftest": (_SELFTEST + ("SR_HLE_AOT_BOUNDARY_TEST",), ()),
    "media-selftest": (_MAKE + ("SR_MPEG_MEDIA_SELFTEST",), ("-std=c11",)),
    "trace": (_MAKE + ("SR_INSTRUCTION_TRACE",), ()),
    "nan-trap": (_MAKE + ("SR_NAN_TRAP",), ()),
    "stack-census": (_MAKE + ("SR_STACK_CENSUS_ENABLED",), ()),
    "perf-aot": (_MAKE[:-1] + ("PERF_AOT_INSTRUCTIONS=1",), ()),
    "android-cmake": (("SR_SDL3VK", "SR_FLIGHT_RECORDER_LINKED", "PERF_AOT_INSTRUCTIONS=0", "SR_PUBLIC_SAFE",
                       "SR_H264_FFMPEG", "NK_UI_WITH_SDL", "NK_UI_WITH_SDL3"), ()),
    "flash0-harness": ((), ("-std=gnu11",)),
}
#: Flags under which a missing declaration is an error on every compiler (GCC 13 only warns).
DECLARATION_ERRORS = ("-Werror=implicit-function-declaration", "-Werror=implicit-int", "-Werror=int-conversion")

REGISTER_API = frozenset({"sr_hle_register", "sr_hle_register_unsupported", "sr_hle_register_entry"})
_REGISTER_CALLS = ("sr_hle_register", "sr_hle_register_unsupported")
_CONDITIONAL = ("if", "ifdef", "ifndef", "else", "elif", "endif")
_TOKEN = re.compile(
    r"""[A-Za-z_]\w*|0[xX][0-9a-fA-F]+[uUlL]*|\d+\.?\d*(?:[eE][+-]?\d+)?[fFuUlL]*|\.\d+[fF]?"""
    r"""|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'|->|\+\+|--|<<=?|>>=?|[<>=!]=|&&|\|\||[-+*/%&|^]=|\.\.\.|##|\S"""
)
_COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)


@dataclass
class Report:
    errors: list[str] = field(default_factory=list)
    static_removed: list[str] = field(default_factory=list)
    made_inline: list[str] = field(default_factory=list)
    regrouped: list[str] = field(default_factory=list)
    new_glue: list[str] = field(default_factory=list)
    missing_comments: list[str] = field(default_factory=list)
    moved_to: dict[str, str] = field(default_factory=dict)
    notes: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.errors


# --------------------------------------------------------------------------------------
# Item classification


def _directive(item: Item) -> str:
    word = re.match(r"#\s*([A-Za-z_]\w*)", item.text)
    return word.group(1) if word else ""


def _is_declaration_glue(item: Item) -> bool:
    """A prototype, an ``extern`` declaration or an ``#include``: allowed to come and go."""
    if item.kind == "pp":
        return _directive(item) == "include"
    if item.kind != "decl":
        return False
    core = blank_comments_and_literals(item.text).strip()
    if core.startswith("extern"):
        return True
    return not is_object_definition(item) and not re.match(r"(typedef|static\s+(?:enum|struct|union)|enum|struct|union)\b",
                                                         core)


def _category(item: Item) -> str:
    """``content`` (must move intact), ``conditional`` (checked through ``cond``) or ``glue``."""
    if item.kind == "guard":
        return "glue"
    if item.kind == "pp":
        if _directive(item) in _CONDITIONAL:
            return "conditional"
        return "glue" if _directive(item) == "include" else "content"
    if _is_declaration_glue(item):
        return "glue"
    return "content"


def _tokens(text: str) -> list[str]:
    return _TOKEN.findall(blank_comments_and_literals_keep_strings(text))


def blank_comments_and_literals_keep_strings(text: str) -> str:
    """``text`` with comments blanked and string literals kept, for tokenizing."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        ch, nxt = text[i], text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            end = text.find("\n", i)
            i = n if end < 0 else end
            out.append(" ")
        elif ch == "/" and nxt == "*":
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            out.append(" ")
        elif ch in "\"'":
            j = i + 1
            while j < n and text[j] != ch and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def string_literals(units: list[Unit]) -> collections.Counter:
    """Every string literal outside comments and preprocessor lines, across ``units``."""
    found: collections.Counter = collections.Counter()
    for _, text in units:
        for item in split_items(text):
            if item.kind in ("pp", "macro"):
                continue
            found.update(t for t in _tokens(item.text) if t.startswith('"'))
    return found


def comments(units: list[Unit]) -> collections.Counter:
    found: collections.Counter = collections.Counter()
    for _, text in units:
        stripped = blank_comments_and_literals(text)
        for match in _COMMENT.finditer(text):
            # Only real comments: the blanked view has spaces where a comment was.
            if stripped[match.start():match.end()].strip() == "":
                found[" ".join(match.group(0).split())] += 1
    return found


def _calls_registration(tokens: list[str], registering: set[str]) -> bool:
    for k, token in enumerate(tokens[:-1]):
        if tokens[k + 1] == "(" and (token in _REGISTER_CALLS or token in registering):
            return True
    return False


def registering_functions(functions: dict[str, str]) -> set[str]:
    """Functions that register NIDs, directly or through other registering functions."""
    tokens = {name: _tokens(body) for name, body in functions.items()}
    registering: set[str] = set()
    changed = True
    while changed:
        changed = False
        for name, toks in tokens.items():
            if name in REGISTER_API or name in registering:
                continue
            body = toks[toks.index("{"):] if "{" in toks else toks
            if _calls_registration(body, registering):
                registering.add(name)
                changed = True
    return registering


def _registration_only(body_tokens: list[str], registering: set[str]) -> bool:
    """Whether a function body holds only registration calls and calls to registration functions."""
    statements: list[list[str]] = []
    current: list[str] = []
    for token in body_tokens:
        current.append(token)
        if token == ";":
            statements.append(current)
            current = []
    if current:
        return False
    for statement in statements:
        head = statement[0]
        if head in _REGISTER_CALLS and statement[1] == "(" and statement[-2] == ")":
            continue
        if head in registering and statement[1:] == ["(", ")", ";"]:
            continue
        return False
    return True


def _function_body_tokens(tokens: list[str]) -> list[str]:
    start = tokens.index("{")
    return tokens[start + 1:-1]


# --------------------------------------------------------------------------------------
# L1


def unit_items(path: str, text: str) -> list[Item]:
    """``split_items``, with a header's include guard removed from every item's conditions.

    The guard (``#ifndef X`` and ``#define X`` first, ``#endif`` last) wraps the whole header
    and changes no item's meaning, so an item moved into a header keeps the conditions it had.
    The guard's own ``#define`` becomes an ``#include``-like glue line.
    """
    items = split_items(text)
    if not path.endswith(".h"):
        return items
    pps = [item for item in items if item.kind in ("pp", "macro")]
    if len(pps) < 3:
        return items
    first, second, last = pps[0], pps[1], pps[-1]
    guard = re.fullmatch(r"#\s*ifndef\s+(\w+)", " ".join(first.text.split()))
    if not (guard and second.kind == "macro" and second.names == [guard.group(1)]
            and _directive(last) == "endif" and items[-1] is last):
        return items
    outer = " ".join(first.text.split())
    for item in items:
        if item.cond[:1] == (outer,):
            item.cond = item.cond[1:]
    second.kind = "guard"  # glue, like an #include
    return items


def compare_items(base: list[Unit], new: list[Unit]) -> Report:
    report = Report()
    base_items = [(path, item) for path, text in base for item in unit_items(path, text)]
    new_items = [(path, item) for path, text in new for item in unit_items(path, text)]

    def functions(items: list[tuple[str, Item]]) -> dict[str, str]:
        out: dict[str, str] = {}
        for _, item in items:
            if item.kind == "func" and item.names:
                out.setdefault(item.names[0], "")
                out[item.names[0]] += item.text + "\n"
        return out

    base_registering = registering_functions(functions(base_items))
    new_registering = registering_functions(functions(new_items))

    new_by_text: dict[str, list[tuple[str, Item]]] = collections.defaultdict(list)
    for path, item in new_items:
        if _category(item) == "content":
            new_by_text[item.text].append((path, item))
    used_new: collections.Counter = collections.Counter()
    groups: dict[str, list[tuple[str, Item]]] = collections.defaultdict(list)
    for path, item in base_items:
        if _category(item) == "content":
            groups[item.text].append((path, item))
    base_texts = set(groups)

    for text_key, members in groups.items():
        path, item = members[0]
        name = item.names[0] if item.names else item.text.split("\n", 1)[0][:60]
        base_conds = collections.Counter(member.cond for _, member in members)
        if item.kind == "macro":
            exact = new_by_text.get(text_key, [])
            if not exact:
                report.errors.append(f"{path}:{item.line}: macro {name} is missing or changed in the new sources")
            for new_path, new_item in exact:
                if new_item.cond not in base_conds:
                    report.errors.append(
                        f"{new_path}:{new_item.line}: macro {name} is under {list(new_item.cond)}, "
                        f"was {[list(c) for c in base_conds]}")
            used_new[text_key] = len(exact)
            continue
        key = text_key
        candidates = new_by_text.get(key, [])
        removed_static = made_inline = False
        if not candidates and item.is_static:
            key = re.sub(r"^static\s+", "", text_key, count=1)
            candidates = new_by_text.get(key, [])
            removed_static = bool(candidates)
        if not candidates and item.is_static and item.kind == "func" and not re.match(r"static\s+inline\b", text_key):
            # A small helper every subsystem calls may move to a private header as `static inline`:
            # it keeps internal linkage and stays inlinable, where dropping `static` would not.
            key = re.sub(r"^static\s+", "static inline ", text_key, count=1)
            candidates = [c for c in new_by_text.get(key, []) if c[0].endswith(".h")]
            made_inline = bool(candidates)
        if item.kind == "func" and name in base_registering and len(candidates) != len(members):
            report.regrouped.append(name)
            continue
        if not candidates:
            report.errors.append(f"{path}:{item.line}: {item.kind} {name} is missing or changed in the new sources")
            continue
        if len(candidates) != len(members):
            where = ", ".join(f"{p}:{i.line}" for p, i in candidates)
            report.errors.append(
                f"{path}:{item.line}: {item.kind} {name} appears {len(candidates)} times in the new sources "
                f"({where}), {len(members)} in the base")
            continue
        used_new[key] = len(candidates)
        if collections.Counter(c.cond for _, c in candidates) != base_conds:
            new_path, new_item = candidates[0]
            report.errors.append(
                f"{new_path}:{new_item.line}: {item.kind} {name} is under {list(new_item.cond)}, "
                f"was {[list(c) for c in base_conds]}")
        new_path = candidates[0][0]
        if item.kind in ("func", "macro_call") or is_object_definition(item):
            report.moved_to[name] = new_path
        if removed_static:
            report.static_removed.append(f"{name} ({new_path})")
        if made_inline:
            report.made_inline.append(f"{name} ({new_path})")

    for path, item in new_items:
        category = _category(item)
        if category != "content" or used_new[item.text]:
            continue
        name = item.names[0] if item.names else item.text.split("\n", 1)[0][:60]
        if item.kind == "func" and name in report.regrouped:
            continue
        if item.kind == "func" and name in new_registering:
            if _registration_only(_function_body_tokens(_tokens(item.text)), new_registering):
                report.new_glue.append(f"{name} ({path})")
            else:
                report.errors.append(
                    f"{path}:{item.line}: new function {name} does more than register NIDs; only registration "
                    "calls and calls to registration functions may appear in code with no base counterpart")
            continue
        if item.kind == "macro" and item.text in base_texts:
            continue
        report.errors.append(f"{path}:{item.line}: {item.kind} {name} has no counterpart in the base sources")

    _check_static_removals(report, new)
    _check_headers(report, new)

    base_strings, new_strings = string_literals(base), string_literals(new)
    if base_strings != new_strings:
        lost = sorted((base_strings - new_strings).elements())[:10]
        gained = sorted((new_strings - base_strings).elements())[:10]
        report.errors.append(f"string literals changed: lost {lost}, gained {gained}")

    missing = comments(base) - comments(new)
    report.missing_comments = sorted(missing.elements())
    return report


def _check_static_removals(report: Report, new: list[Unit]) -> None:
    blanked = {path: blank_comments_and_literals(text) for path, text in new}
    for entry in report.static_removed:
        name, home = entry.rsplit(" (", 1)
        home = home.rstrip(")")
        pattern = re.compile(rf"\b{re.escape(name)}\b")
        users = [p for p, text in blanked.items() if p != home and not p.endswith(".h") and pattern.search(text)]
        if not users:
            report.errors.append(f"{home}: `static` was dropped from {name}, which no other HLE file uses")
        declared = [p for p, text in blanked.items() if p.endswith(".h") and re.search(
            rf"\bextern\b[^;]*\b{re.escape(name)}\b|\b{re.escape(name)}\s*\([^;{{]*\)\s*;", text)]
        if len(declared) != 1:
            report.errors.append(f"{name}: declared in {len(declared)} private headers, not exactly one")


def _check_headers(report: Report, new: list[Unit]) -> None:
    blanked = [blank_comments_and_literals(text) for _, text in new]
    for path, text in new:
        if not path.endswith(".h"):
            continue
        for item in split_items(text):
            name = item.names[0] if item.names else "?"
            if is_object_definition(item):
                report.errors.append(f"{path}:{item.line}: header defines object {name}; each file would get a copy")
            if item.kind != "func":
                continue
            head = item.text[:item.text.index("{")]
            if not re.match(r"\s*static\s+inline\b", head):
                report.errors.append(f"{path}:{item.line}: header defines function {name} without `static inline`")
            elif re.search(r"\bstatic\b", blank_comments_and_literals(item.text[item.text.index("{"):])):
                report.errors.append(
                    f"{path}:{item.line}: inline function {name} has a `static` local; each file would get one")
            # Each file gets its own copy, so taking the address would give different pointers.
            if any(re.search(rf"\b{re.escape(name)}\b(?!\s*\()", text_) for text_ in blanked):
                report.errors.append(f"{path}:{item.line}: the address of inline function {name} is taken")


# --------------------------------------------------------------------------------------
# L2 and L3


def _compile_flags(tree: Path, config: tuple[tuple[str, ...], tuple[str, ...]]) -> list[str]:
    defines, flags = config
    return [*flags, *(f"-D{d}" for d in defines), *(f"-I{tree / d}" for d in INCLUDE_DIRS)]


def syntax_errors(tree: Path, relative: str, config: tuple[tuple[str, ...], tuple[str, ...]], cc: str) -> str:
    """Compiler errors for ``relative`` with missing declarations made errors; empty when it compiles."""
    command = [cc, "-fsyntax-only", *DECLARATION_ERRORS, *_compile_flags(tree, config), str(tree / relative)]
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return "" if result.returncode == 0 else result.stderr.strip()[-1500:]


def preprocess_unit(tree: Path, relative: str, config: tuple[tuple[str, ...], tuple[str, ...]], cc: str,
                    keep: tuple[str, ...]) -> str:
    """The preprocessed text of ``relative`` (in ``tree``) that came from the files in ``keep``."""
    command = [cc, "-E", *_compile_flags(tree, config), str(tree / relative)]
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode != 0:
        raise RuntimeError(f"{' '.join(command)} failed:\n{result.stderr[-2000:]}")
    wanted = {Path(tree / k).resolve().as_posix().lower() for k in keep}
    out: list[str] = []
    current_kept = False
    marker = re.compile(r'^#\s*(?:line\s+)?\d+\s+"((?:\\.|[^"\\])*)"')
    for line in result.stdout.split("\n"):
        m = marker.match(line)
        if m:
            name = m.group(1).replace("\\\\", "\\")
            current_kept = Path(name).resolve().as_posix().lower() in wanted
            out.append("")
            continue
        out.append(line if current_kept else "")
    return "\n".join(out)


def preprocessed_definitions(tree: Path, units: list[str], headers: tuple[str, ...],
                             config: tuple[tuple[str, ...], tuple[str, ...]], cc: str) -> dict[str, list[str]]:
    """Name -> token list of every function, object and type, from every unit in one configuration."""
    found: dict[str, list[str]] = {}
    for relative in units:
        text = preprocess_unit(tree, relative, config, cc, (relative,) + headers)
        for item in split_items(text):
            if not item.names or item.kind != "func" and not (item.kind == "decl" and not _is_declaration_glue(item)):
                continue
            tokens = _TOKEN.findall(item.text)
            previous = found.get(item.names[0])
            if previous is not None and previous != tokens:
                found[item.names[0] + "@" + relative] = tokens
            else:
                found[item.names[0]] = tokens
    return found


def flatten_init(definitions: dict[str, list[str]], root: str = "sr_hle_init") -> list[str]:
    """``root``'s body with every no-argument call to a registration function inlined."""
    bodies = {name: " ".join(tokens) for name, tokens in definitions.items() if "{" in tokens}
    registering = registering_functions(bodies)

    def flatten(name: str, depth: int) -> list[str]:
        if depth > 16:
            raise RuntimeError(f"registration functions nest more than 16 deep at {name}")
        body = _function_body_tokens(definitions[name])
        out: list[str] = []
        k = 0
        while k < len(body):
            token = body[k]
            if token in registering and body[k + 1:k + 4] == ["(", ")", ";"] and token in definitions:
                inner = _function_body_tokens(definitions[token])
                if "goto" in inner:
                    raise RuntimeError(f"cannot inline {token}: it jumps")
                # A body that returns (a run-once guard) is inlined as a scoped block, so its
                # `return` still means "leave this registration function", not "leave init".
                scoped = "return" in inner
                out += (["@{"] if scoped else []) + flatten(token, depth + 1) + (["@}"] if scoped else [])
                k += 4
                continue
            out.append(token)
            k += 1
        return out

    if root not in definitions:
        raise RuntimeError(f"{root} is not defined in this configuration")
    return flatten(root, 0)


def registration_roots(definitions: dict[str, list[str]]) -> list[str]:
    """Registering functions no other registering function calls: ``sr_hle_init`` and any
    other entry point that registers (a selftest hook that re-runs one helper, for example)."""
    bodies = {name: " ".join(tokens) for name, tokens in definitions.items() if "{" in tokens}
    registering = registering_functions(bodies)
    called: set[str] = set()
    for name in registering:
        tokens = _function_body_tokens(definitions[name])
        called |= {t for k, t in enumerate(tokens[:-1]) if t in registering and tokens[k + 1] == "("}
    return sorted(registering - called)


def compare_preprocessed(base_tree: Path, base_units: list[str], base_headers: tuple[str, ...],
                         new_tree: Path, new_units: list[str], new_headers: tuple[str, ...],
                         report: Report, cc: str,
                         configs: dict[str, tuple[tuple[str, ...], tuple[str, ...]]]) -> None:
    removed = {entry.rsplit(" (", 1)[0] for entry in report.static_removed}
    inlined = {entry.rsplit(" (", 1)[0] for entry in report.made_inline}
    glue = {entry.rsplit(" (", 1)[0] for entry in report.new_glue}
    for config, flags in configs.items():
        base = preprocessed_definitions(base_tree, base_units, base_headers, flags, cc)
        new = preprocessed_definitions(new_tree, new_units, new_headers, flags, cc)
        # L5: a declaration missing from the private header must fail here, not compile into an
        # implicit `int` on a compiler that only warns about it.
        for relative in new_units:
            errors = syntax_errors(new_tree, relative, flags, cc)
            if errors and not (relative in base_units and syntax_errors(base_tree, relative, flags, cc)):
                report.errors.append(f"[{config}] {relative} does not compile with declarations enforced:\n{errors}")
        regrouped = set(report.regrouped)
        for name, tokens in base.items():
            if name in regrouped:
                continue
            other = new.get(name)
            if other is None:
                report.errors.append(f"[{config}] {name} is defined in the base but not in the new sources")
                continue
            if name in removed and tokens[:1] == ["static"]:
                tokens = tokens[1:]
            if name in inlined and other[:2] == ["static", "inline"]:
                other = other[:1] + other[2:]
            if other != tokens:
                k = next((i for i, (a, b) in enumerate(zip(tokens, other, strict=False)) if a != b), min(len(tokens), len(other)))
                report.errors.append(
                    f"[{config}] {name} preprocesses differently: base {' '.join(tokens[max(0, k - 4):k + 6])!r} "
                    f"vs new {' '.join(other[max(0, k - 4):k + 6])!r}")
        for name in new:
            if name not in base and name not in glue and name.split("@", 1)[0] not in regrouped:
                report.errors.append(f"[{config}] {name} is defined only in the new sources")
        for root in registration_roots(base):
            try:
                if flatten_init(base, root) != flatten_init(new, root):
                    report.errors.append(f"[{config}] {root}'s flattened registration sequence differs")
            except RuntimeError as exc:
                report.errors.append(f"[{config}] registration sequence of {root}: {exc}")


# --------------------------------------------------------------------------------------
# CLI


def _git(*args: str) -> str:
    result = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, encoding="utf-8")
    if result.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def base_unit_paths(rev: str) -> tuple[list[str], list[str]]:
    listed = _git("ls-tree", "-r", "--name-only", rev, "--", "src/rt").split()
    units = [p for p in listed if re.fullmatch(r"src/rt/hle(?:_[a-z0-9_]+)?\.c", p)
             and p not in hle_sources.NOT_RUNTIME]
    units.sort(key=lambda p: (p != "src/rt/hle.c", p))
    headers = [p for p in ("src/rt/hle_internal.h", "src/rt/hle_power.h") if p in listed]
    return units, headers


def _extract_tree(rev: str, into: Path) -> None:
    archive = subprocess.run(["git", "archive", "--format=tar", rev, "src"], cwd=ROOT, capture_output=True)
    if archive.returncode != 0:
        raise RuntimeError(f"git archive {rev} failed: {archive.stderr.decode(errors='replace')}")
    with tarfile.open(fileobj=io.BytesIO(archive.stdout)) as tar:
        tar.extractall(into, filter="data")


def manifest_differences(base_text: str, new_text: str) -> list[str]:
    import hle_manifest

    def strip(manifest: dict) -> list[dict]:
        return [{k: v for k, v in r.items() if k != "owner_file"} for r in manifest["registrations"]]

    out = []
    base_manifest, new_manifest = hle_manifest.build_manifest(base_text), hle_manifest.build_manifest(new_text)
    if strip(base_manifest) != strip(new_manifest):
        out.append("HLE manifest registrations differ (beyond owner_file)")
    if base_manifest["findings"] != new_manifest["findings"]:
        out.append("HLE manifest findings differ")
    base_scopes = {k: v["reachable_from"] for k, v in hle_manifest.registration_scopes(base_text).items()}
    new_scopes = {k: v["reachable_from"] for k, v in hle_manifest.registration_scopes(new_text).items()}
    if base_scopes != new_scopes:
        changed = sorted(f"0x{k:08x}" for k in set(base_scopes) | set(new_scopes) if base_scopes.get(k) != new_scopes.get(k))
        out.append(f"production/selftest reachability changed for {changed[:10]}")
    return out


def run(rev: str, cc: str | None, configs: dict[str, tuple[str, ...]]) -> Report:
    base_paths, base_headers = base_unit_paths(rev)
    base = [(p, _git("show", f"{rev}:{p}")) for p in base_paths + base_headers]
    new = hle_sources.sources(headers=True)
    report = compare_items(base, new)
    base_c = "\n".join(text for p, text in base if p.endswith(".c"))
    new_c = hle_sources.text(headers=False)
    report.errors += manifest_differences(base_c, new_c)
    baseline = "tools/import_audit_baseline.json"
    if _git("show", f"{rev}:{baseline}") != (ROOT / baseline).read_text(encoding="utf-8"):
        report.errors.append(f"{baseline} differs from {rev}'s")
    if cc is None:
        report.notes.append("L2/L3 NOT_RUN: no C compiler given")
        return report
    with tempfile.TemporaryDirectory() as tmp:
        base_tree = Path(tmp)
        _extract_tree(rev, base_tree)
        compare_preprocessed(base_tree, base_paths, tuple(base_headers), ROOT,
                             list(hle_sources.RUNTIME_HLE_SOURCES), hle_sources.HLE_PRIVATE_HEADERS,
                             report, cc, configs)
    return report


def render(report: Report, rev: str) -> str:
    lines = [f"# HLE move proof against {rev}", "", f"RESULT: {'PASS' if report.ok else 'FAIL'}", ""]
    sections = (
        ("Errors", report.errors),
        ("Static removed (name, new home)", report.static_removed),
        ("Moved to a private header as static inline (name, header)", report.made_inline),
        ("Registration functions regrouped (proved by L3)", report.regrouped),
        ("New registration-only functions", report.new_glue),
        ("Base comments not found in the new sources (review)", report.missing_comments),
        ("Notes", report.notes),
    )
    for title, entries in sections:
        lines.append(f"## {title} ({len(entries)})")
        lines += [f"- {e}" for e in entries] or ["- none"]
        lines.append("")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--base", required=True, help="the revision the move starts from")
    parser.add_argument("--cc", default=shutil.which("gcc"), help="C compiler for L2/L3 (default: gcc on PATH)")
    parser.add_argument("--no-preprocess", action="store_true", help="skip L2/L3 (reported NOT_RUN)")
    parser.add_argument("--config", action="append", choices=sorted(CONFIGS), help="limit L2/L3 to these")
    parser.add_argument("--report", type=Path, help="write the Markdown report here too")
    parser.add_argument("--map", type=Path, help="write the moved-name -> new file map as JSON")
    args = parser.parse_args(argv)
    configs = {k: v for k, v in CONFIGS.items() if not args.config or k in args.config}
    report = run(args.base, None if args.no_preprocess else args.cc, configs)
    text = render(report, args.base)
    print(text)
    if args.report:
        args.report.write_text(text, encoding="utf-8")
    if args.map:
        args.map.write_text(json.dumps(dict(sorted(report.moved_to.items())), indent=1) + "\n", encoding="utf-8")
    return 0 if report.ok else 1


if __name__ == "__main__":
    sys.exit(main())
