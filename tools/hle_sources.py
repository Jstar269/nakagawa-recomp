#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The runtime HLE sources, in one place (issue #307).

Every tool that reads the HLE implementation reads it through this module, never through a
path of its own. Splitting ``src/rt/hle.c`` into subsystem files is only safe if no check is
left scanning one file for a pattern that has moved to another: such a check would keep
passing on text that no longer holds the code it was written for.

``RUNTIME_HLE_SOURCES`` lists the C files that make up the runtime HLE, ``src/rt/hle.c``
first. The readers fail closed: a missing, unreadable or empty file, or a set that has lost
``sr_hle_init``, is an error rather than an empty text that every "not in" assertion would
accept. ``tools/test_hle_sources.py`` keeps the list equal to the ``src/rt/hle*.c`` files
on disk.

The module also carries the top-level C item splitter the HLE tools share
(:func:`split_items`): it cuts a translation unit into functions, declarations and
preprocessor lines, each with its leading comment and the conditionals around it, and
joins back to the exact input.

CLI: ``python tools/hle_sources.py`` prints the list; ``--defining-file NAME`` prints the
file that defines a function or object.
"""

from __future__ import annotations

import argparse
import ast
from dataclasses import dataclass, field
import functools
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

#: The C files of the runtime HLE, repo-relative. ``src/rt/hle.c`` is first; the order is
#: the order :func:`text` joins them in, and carries no other meaning.
RUNTIME_HLE_SOURCES: tuple[str, ...] = (
    "src/rt/hle.c",
    "src/rt/hle_power.c",
)

#: Headers private to the runtime HLE files. :func:`text` includes them by default, so a
#: whole-source scan also covers declarations and inline helpers kept there.
HLE_PRIVATE_HEADERS: tuple[str, ...] = (
    "src/rt/hle_power.h",
)

#: ``src/rt/hle*.c`` files that are not part of the runtime HLE.
NOT_RUNTIME: tuple[str, ...] = (
    "src/rt/hle_thread_selftest.c",
)

#: The function every complete runtime HLE source set defines exactly once.
INIT_SIGNATURE = "void sr_hle_init(void)"

#: The storage-class `static` of a declaration, after any leading GNU attributes
#: (``__attribute__((format(printf, 2, 3))) static uint32_t f(...)``).
STATIC_SPECIFIER = re.compile(r"^(\s*(?:__attribute__\s*\(\((?:[^()]|\([^()]*\))*\)\)\s*)*)static\b\s*")

#: A string constant naming a runtime HLE source by path: ``hle.c``, ``src/rt/hle_io.c``, or a glob
#: (``hle*.c``) or f-string (``f"hle_{name}.c"``, read with ``*`` for each field) that matches one.
_HLE_SOURCE_NAME_RE = re.compile(r"(?:^|/)hle(?:[_*][a-z0-9_*]*)?\.c$")


class HleSourcesError(RuntimeError):
    """The runtime HLE sources could not be read completely."""


# --------------------------------------------------------------------------------------
# Reading the sources


def _read(root: Path, relative: str) -> str:
    path = root / relative
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise HleSourcesError(f"{relative}: cannot be read: {exc}") from exc
    if not text.strip():
        raise HleSourcesError(f"{relative}: is empty")
    return text


def sources(root: Path = ROOT, *, headers: bool = False) -> list[tuple[str, str]]:
    """``(path, text)`` for every runtime HLE source, in list order; headers last when asked."""
    paths = RUNTIME_HLE_SOURCES + (HLE_PRIVATE_HEADERS if headers else ())
    units = [(relative, _read(root, relative)) for relative in paths]
    joined = "\n".join(text for _, text in units)
    if joined.count(INIT_SIGNATURE) != 1:
        raise HleSourcesError(
            f"the runtime HLE sources define `{INIT_SIGNATURE}` {joined.count(INIT_SIGNATURE)} times, not once; "
            "the source list is incomplete or wrong"
        )
    return units


def text(root: Path = ROOT, *, headers: bool = True) -> str:
    """Every runtime HLE source (and, by default, its private headers) joined by newlines.

    This is the text for whole-source scans: a pattern that must appear, or must never
    appear, anywhere in the runtime HLE. Order between files carries no meaning, so compare
    positions only inside one function (:func:`function_text`).
    """
    return "\n".join(body for _, body in sources(root, headers=headers))


# --------------------------------------------------------------------------------------
# Top-level C items

_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_KEYWORDS = frozenset(
    "auto break case char const continue default do double else enum extern float for goto if inline int "
    "long register restrict return short signed sizeof static struct switch typedef union unsigned void "
    "volatile while _Alignas _Alignof _Atomic _Bool _Complex _Imaginary _Noreturn _Static_assert "
    "_Thread_local __attribute__ __inline __inline__ __restrict".split()
)
_MACRO_CALL = re.compile(r"[A-Z_][A-Z0-9_]*\s*\(.*\)", re.DOTALL)


def blank_comments_and_literals(source: str) -> str:
    """``source`` with comments and the contents of string and character literals blanked.

    Offsets and newlines are kept, and so are the quote characters, so the result lines up
    with the input character for character.
    """
    out = list(source)
    i, n = 0, len(source)
    while i < n:
        ch = source[i]
        nxt = source[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            end = source.find("\n", i)
            end = n if end < 0 else end
            for k in range(i, end):
                out[k] = " "
            i = end
        elif ch == "/" and nxt == "*":
            end = source.find("*/", i + 2)
            end = n if end < 0 else end + 2
            for k in range(i, end):
                if source[k] != "\n":
                    out[k] = " "
            i = end
        elif ch in "\"'":
            j = i + 1
            while j < n and source[j] != ch and source[j] != "\n":
                j += 2 if source[j] == "\\" else 1
            j = min(j + 1, n)
            for k in range(i + 1, j - 1):
                out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


@dataclass
class Item:
    """One top-level item of a C translation unit.

    ``kind`` is ``func`` (a function definition), ``decl`` (a declaration or definition
    ending in ``;``), ``macro`` (a ``#define``), ``pp`` (any other preprocessor line) or
    ``macro_call`` (a top-level macro invocation without a semicolon). ``lead`` is the
    comment and blank text between the previous item and this one, ``text`` the item
    itself, ``cond`` the stack of enclosing ``#if`` conditions (each as written, with any
    ``#else``/``#elif`` taken appended), ``names`` the identifiers it defines.
    """

    kind: str
    start: int
    end: int
    lead: str = ""
    text: str = ""
    line: int = 0
    names: list[str] = field(default_factory=list)
    cond: tuple[str, ...] = ()
    is_static: bool = False


def _at_line_start(blanked: str, index: int) -> bool:
    line_start = blanked.rfind("\n", 0, index) + 1
    return not blanked[line_start:index].strip()


def _directive_end(source: str, index: int) -> int:
    """End offset of the preprocessor line starting at ``index``, following ``\\`` continuations."""
    n = len(source)
    j = index
    while True:
        end = source.find("\n", j)
        end = n if end < 0 else end
        if end < n and source[end - 1:end] == "\\":
            j = end + 1
            continue
        return end


def split_items(source: str) -> list[Item]:
    """Cut ``source`` into top-level items; ``"".join(lead + text)`` plus the tail gives it back."""
    blanked = blank_comments_and_literals(source)
    n = len(source)
    items: list[Item] = []
    cond: list[str] = []
    i = 0
    while i < n:
        while i < n and blanked[i] in " \t\r\n\f\v":
            i += 1
        if i >= n:
            break
        start = i
        if blanked[i] == "#" and _at_line_start(blanked, i):
            end = _directive_end(source, i)
            directive = " ".join(source[start:end].split())
            word = _IDENT.match(directive.lstrip("#").strip())
            keyword = word.group(0) if word else ""
            item = Item("pp", start, end)
            if keyword in ("if", "ifdef", "ifndef"):
                item.cond = tuple(cond)
                cond.append(directive)
            elif keyword in ("else", "elif"):
                item.cond = tuple(cond[:-1])
                if cond:
                    cond[-1] = f"{cond[-1]} | {directive}"
            elif keyword == "endif":
                if cond:
                    cond.pop()
                item.cond = tuple(cond)
            else:
                item.cond = tuple(cond)
                if keyword == "define":
                    name = _IDENT.match(directive.lstrip("#").strip()[len("define"):].lstrip())
                    if name:
                        item.kind = "macro"
                        item.names = [name.group(0)]
            items.append(item)
            i = end
            continue
        end, is_function = _scan_item(source, blanked, start)
        core = blanked[start:end]
        kind = "func" if is_function else "decl"
        if not is_function and _MACRO_CALL.fullmatch(core.strip()):
            kind = "macro_call"
        item = Item(kind, start, end, cond=tuple(cond))
        _name_item(item, core)
        items.append(item)
        i = end
    position = 0
    for item in items:
        item.lead = source[position:item.start]
        item.text = source[item.start:item.end]
        item.line = source.count("\n", 0, item.start) + 1
        position = item.end
    return items


def _scan_item(source: str, blanked: str, start: int) -> tuple[int, bool]:
    """End of the non-preprocessor item at ``start`` and whether it is a function definition."""
    n = len(source)
    depth = paren = 0
    is_function = False
    j = start
    while j < n:
        ch = blanked[j]
        if ch == "#" and _at_line_start(blanked, j):
            j = _directive_end(source, j)
            continue
        if ch == "(":
            paren += 1
        elif ch == ")":
            paren -= 1
        elif ch == "{":
            if depth == 0 and paren == 0:
                k = j - 1
                while k > start and blanked[k] in " \t\r\n":
                    k -= 1
                if blanked[k] == ")" and "=" not in blanked[start:j]:
                    is_function = True
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0 and is_function:
                return j + 1, True
        elif ch == ";" and depth == 0 and paren == 0:
            return j + 1, False
        elif ch == "\n" and depth == 0 and paren == 0 and _MACRO_CALL.fullmatch(blanked[start:j].strip()):
            return j, False
        j += 1
    return n, is_function


def _last_top_paren(head: str) -> int:
    depth = 0
    last = -1
    for k, ch in enumerate(head):
        if ch == "(":
            if depth == 0:
                last = k
            depth += 1
        elif ch == ")":
            depth -= 1
    return last if last >= 0 else len(head)


def _name_item(item: Item, core: str) -> None:
    if item.kind == "func":
        head = core[:core.index("{")]
        words = _IDENT.findall(head[:_last_top_paren(head)])
        item.names = [words[-1]] if words else []
        item.is_static = STATIC_SPECIFIER.match(head) is not None
    elif item.kind == "decl":
        item.names = _declared_names(core)
        item.is_static = STATIC_SPECIFIER.match(core) is not None
    elif item.kind == "macro_call":
        match = re.match(r"\s*[A-Z_][A-Z0-9_]*\s*\(\s*([A-Za-z_]\w*)", core)
        if match:
            item.names = [match.group(1)]


def _declared_names(core: str) -> list[str]:
    body = core.strip().rstrip(";")
    if re.match(r"(typedef\s+)?(enum|struct|union)\b", body):
        names = []
        tag = re.match(r"(?:typedef\s+)?(?:enum|struct|union)\s+([A-Za-z_]\w*)", body)
        if tag:
            names.append(tag.group(1))
        if "{" in body and "}" in body:
            if re.match(r"(typedef\s+)?enum\b", body):
                inner = body[body.index("{") + 1:body.rindex("}")]
                names += [m.group(0) for part in inner.split(",") if (m := _IDENT.match(part.strip()))]
            names += [w for w in _IDENT.findall(body[body.rindex("}") + 1:]) if w not in _KEYWORDS]
        return names
    parts: list[str] = []
    current: list[str] = []
    depth = 0
    skipping = False
    for ch in body:
        if ch in "({[":
            depth += 1
        elif ch in ")}]":
            depth -= 1
        if depth == 0 and ch == "=":
            skipping = True
            continue
        if depth == 0 and ch == ",":
            parts.append("".join(current))
            current, skipping = [], False
            continue
        if not skipping:
            current.append(ch)
    parts.append("".join(current))
    names = []
    for part in parts:
        pointer = re.search(r"\(\s*\*\s*([A-Za-z_]\w*)\s*\)", part)
        if pointer:
            names.append(pointer.group(1))
            continue
        call = re.search(r"([A-Za-z_]\w*)\s*\(", part)
        if call:
            names.append(call.group(1))
            continue
        words = [w for w in _IDENT.findall(re.sub(r"\[[^\]]*\]", "", part)) if w not in _KEYWORDS]
        if words:
            names.append(words[-1])
    return names


def is_object_definition(item: Item) -> bool:
    """Whether a ``decl`` item defines storage (not a prototype, ``extern`` or type)."""
    if item.kind != "decl":
        return False
    core = blank_comments_and_literals(item.text).strip()
    if re.match(r"(extern|typedef)\b", core) or re.match(r"(static\s+)?(enum|struct|union)\b[^=]*\{", core):
        return False
    head = core.split("=", 1)[0]
    return not (re.search(r"[A-Za-z_]\w*\s*\(", head) and not re.search(r"\(\s*\*", head))


# --------------------------------------------------------------------------------------
# Definitions


@functools.lru_cache(maxsize=8)
def _items_of(body: str) -> tuple[Item, ...]:
    return tuple(split_items(body))


def definitions(root: Path = ROOT) -> dict[str, list[str]]:
    """Function and object name -> the runtime HLE files (and private headers) defining it.

    Two ``#ifdef`` arms of one function in one file count once.
    """
    found: dict[str, list[str]] = {}
    for relative, body in sources(root, headers=True):
        for item in _items_of(body):
            if item.kind == "func" or item.kind == "macro_call" or is_object_definition(item):
                for name in item.names:
                    files = found.setdefault(name, [])
                    if relative not in files:
                        files.append(relative)
    return found


def defining_file(name: str, root: Path = ROOT) -> str:
    """The one runtime HLE file that defines ``name``."""
    files = definitions(root).get(name, [])
    if len(files) != 1:
        raise HleSourcesError(f"`{name}` is defined in {len(files)} runtime HLE files ({files}), not exactly one")
    return files[0]


def function_text(signature: str, root: Path = ROOT) -> str:
    """The one function definition whose text contains ``signature``, from its start to its brace.

    ``signature`` is any text unique to the definition's head, for example
    ``"static void flash0_font_sources(Flash0Sources *out) {"`` or ``"sr_vblank_tick(void)"``.
    """
    matches: list[str] = []
    for relative, body in sources(root, headers=True):
        for item in _items_of(body):
            if item.kind == "func" and signature in item.text[:item.text.index("{") + 1]:
                matches.append(f"{relative}:{item.line}")
                found = item.text
    if len(matches) != 1:
        raise HleSourcesError(f"{len(matches)} runtime HLE function definitions match {signature!r}: {matches}")
    return found


# --------------------------------------------------------------------------------------
# Build files


def makefile_hle_srcs(makefile_text: str) -> list[str]:
    """The files of the Makefile's ``HLE_SRCS :=`` assignment, continuation lines included."""
    match = re.search(r"(?m)^HLE_SRCS\s*:=((?:[^\n]*\\\n)*[^\n]*)$", makefile_text)
    if match is None:
        raise HleSourcesError("the Makefile has no `HLE_SRCS :=` assignment")
    return match.group(1).replace("\\\n", " ").split()


def unlisted_hle_files(root: Path = ROOT) -> list[str]:
    """``src/rt/hle*.c`` files that are neither listed runtime sources nor in ``NOT_RUNTIME``."""
    on_disk = {p.relative_to(root).as_posix() for p in (root / "src" / "rt").glob("hle*.c")}
    return sorted(on_disk - set(RUNTIME_HLE_SOURCES) - set(NOT_RUNTIME))


# --------------------------------------------------------------------------------------
# The path-name guard


def hle_source_path_constants(python_source: str, filename: str = "<python>") -> list[tuple[int, str]]:
    """``(line, value)`` of string constants in ``python_source`` that name a runtime HLE source file.

    A constant counts when its whole value, stripped, is a path or file name of the form
    ``hle.c`` / ``hle_<name>.c``: ``ROOT / "src" / "rt" / "hle.c"`` and ``"src/rt/hle_io.c"``
    both count; a sentence that mentions ``src/rt/hle.c`` does not. A regular expression
    (``hle\\.c``), a glob (``hle*.c``) and an f-string (``f"src/rt/hle_{name}.c"``) count
    too. Docstrings are skipped.
    """
    tree = ast.parse(python_source, filename=filename)
    docstrings: set[int] = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
            body = getattr(node, "body", [])
            if body and isinstance(body[0], ast.Expr) and isinstance(body[0].value, ast.Constant):
                docstrings.add(id(body[0].value))
    not_runtime = {Path(path).name for path in NOT_RUNTIME}
    hits: list[tuple[int, str]] = []
    in_fstring: set[int] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.JoinedStr):
            parts = [part.value if isinstance(part, ast.Constant) else "*" for part in node.values]
            in_fstring.update(id(part) for part in node.values)
            if _names_hle_source("".join(parts), not_runtime):
                hits.append((node.lineno, "".join(parts)))
    for node in ast.walk(tree):
        if not (isinstance(node, ast.Constant) and isinstance(node.value, str)) or id(node) in docstrings:
            continue
        if id(node) in in_fstring:
            continue
        if _names_hle_source(node.value, not_runtime):
            hits.append((node.lineno, node.value))
    return sorted(hits)


def _names_hle_source(value: str, not_runtime: set[str]) -> bool:
    value = value.strip().replace("\\", "")
    return " " not in value and bool(_HLE_SOURCE_NAME_RE.search(value)) and value.rsplit("/", 1)[-1] not in not_runtime


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root (default: this checkout)")
    parser.add_argument("--defining-file", metavar="NAME", help="print the file that defines NAME")
    args = parser.parse_args(argv)
    try:
        if args.defining_file:
            print(defining_file(args.defining_file, args.root))
            return 0
        for relative, body in sources(args.root, headers=True):
            print(f"{relative}\t{body.count(chr(10))} lines")
    except HleSourcesError as exc:
        print(f"hle_sources: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
