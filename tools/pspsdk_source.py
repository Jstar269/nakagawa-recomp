#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Bounded, fail-closed extraction of imports and prototypes from pinned PSPSDK."""

from __future__ import annotations

from collections import defaultdict
import os
from pathlib import Path
import re
import subprocess
from typing import Any, Callable

import pspdev_lock

SCHEMA = 1
MAX_SOURCE_FILES = 5000
MAX_SOURCE_FILE_BYTES = 8 * 1024 * 1024
MAX_IMPORTS = 20000
MAX_PROTOTYPES = 50000
MAX_DECLARATION_BYTES = 4096
GIT_TIMEOUT_SECONDS = 10.0
GIT_OUTPUT_BYTES = 64 * 1024

IMPORT_START_RE = re.compile(
    r'^\s*IMPORT_START\s+"(?P<library>[^"\\]+)"\s*,\s*'
    r'(?P<flags>0x[0-9A-Fa-f]{1,8})\s*$'
)
IMPORT_FUNC_RE = re.compile(
    r'^\s*IMPORT_FUNC\s+"(?P<library>[^"\\]+)"\s*,\s*'
    r'(?P<nid>0x[0-9A-Fa-f]{8})\s*,\s*'
    r'(?P<symbol>[A-Za-z_][A-Za-z0-9_]*)\s*$'
)
IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
IDENT_TOKEN_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
RETURN_TYPE_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \t*]*$")


class SyncError(RuntimeError):
    """PSPSDK source violated the narrow extraction or identity contract."""


def _strip_comments(source: str) -> str:
    """Blank C/C++ comments while preserving offsets and quoted literals."""

    out = list(source)
    index = 0
    while index < len(source):
        char = source[index]
        if char in {'"', "'"}:
            quote = char
            index += 1
            while index < len(source):
                if source[index] == "\\":
                    index += 2
                elif source[index] == quote:
                    index += 1
                    break
                else:
                    index += 1
        elif source.startswith("/*", index):
            end = source.find("*/", index + 2)
            end = len(source) if end < 0 else end + 2
            for pos in range(index, end):
                if source[pos] != "\n":
                    out[pos] = " "
            index = end
        elif source.startswith("//", index):
            end = source.find("\n", index + 2)
            end = len(source) if end < 0 else end
            for pos in range(index, end):
                out[pos] = " "
            index = end
        else:
            index += 1
    return "".join(out)


def _read_bounded(path: Path, *, max_bytes: int = MAX_SOURCE_FILE_BYTES) -> str:
    if path.is_symlink():
        raise SyncError(f"{path.name} is a symlink; source scans require ordinary files")
    try:
        with path.open("rb") as stream:
            size = os.fstat(stream.fileno()).st_size
            if size < 0 or size > max_bytes:
                raise SyncError(
                    f"{path.name} is {size} bytes, exceeding the {max_bytes}-byte cap"
                )
            raw = stream.read(max_bytes + 1)
            if len(raw) > max_bytes or stream.read(1):
                raise SyncError(f"{path.name} grew beyond the source cap while reading")
    except SyncError:
        raise
    except OSError as exc:
        raise SyncError(f"cannot read {path.name}: {type(exc).__name__}") from exc
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise SyncError(f"{path.name} is not UTF-8") from exc


def parse_import_assembly(
    source: str, source_file: str
) -> tuple[list[dict[str, Any]], dict[str, str]]:
    """Parse IMPORT_START/IMPORT_FUNC lines and reject every unknown use."""

    clean = _strip_comments(source)
    libraries: dict[str, str] = {}
    functions: list[dict[str, Any]] = []
    for line_number, raw_line in enumerate(clean.splitlines(), 1):
        line = raw_line.strip()
        if "IMPORT_START" in line:
            match = IMPORT_START_RE.fullmatch(raw_line)
            if not match:
                raise SyncError(
                    f"{source_file}:{line_number}: unrecognized IMPORT_START: {line!r}"
                )
            library = match.group("library")
            flags = f"0x{int(match.group('flags'), 16):08x}"
            previous = libraries.get(library)
            if previous is not None and previous != flags:
                raise SyncError(
                    f"{source_file}:{line_number}: library {library!r} has "
                    f"conflicting flags {previous} and {flags}"
                )
            libraries[library] = flags
        if "IMPORT_FUNC" in line:
            match = IMPORT_FUNC_RE.fullmatch(raw_line)
            if not match:
                raise SyncError(
                    f"{source_file}:{line_number}: unrecognized IMPORT_FUNC: {line!r}"
                )
            functions.append(
                {
                    "library": match.group("library"),
                    "nid": f"0x{int(match.group('nid'), 16):08x}",
                    "symbol": match.group("symbol"),
                    "source_file": source_file,
                    "source_line": line_number,
                }
            )
    if functions and not libraries:
        raise SyncError(f"{source_file}: IMPORT_FUNC entries exist without IMPORT_START")
    for function in functions:
        if function["library"] not in libraries:
            raise SyncError(
                f"{source_file}:{function['source_line']}: IMPORT_FUNC library "
                f"{function['library']!r} has no matching IMPORT_START"
            )
    return functions, libraries


def _source_files(root: Path, suffix: str) -> list[Path]:
    root = root.resolve(strict=True)
    src = root / "src"
    if not src.is_dir() or src.is_symlink():
        raise SyncError(f"{root.name} does not contain an ordinary src directory")
    paths: list[Path] = []
    for path in sorted(src.rglob(f"*{suffix}")):
        if path.is_symlink():
            raise SyncError(f"{path.relative_to(root).as_posix()} is a symlink")
        if not path.is_file():
            continue
        try:
            path.resolve(strict=True).relative_to(root)
        except (OSError, ValueError) as exc:
            raise SyncError(f"{path.name} escapes the PSPSDK source root") from exc
        paths.append(path)
        if len(paths) > MAX_SOURCE_FILES:
            raise SyncError(f"more than {MAX_SOURCE_FILES} {suffix} source files")
    return paths


def scan_imports(root: Path) -> tuple[list[dict[str, Any]], dict[str, str]]:
    functions: list[dict[str, Any]] = []
    libraries: dict[str, str] = {}
    seen: dict[tuple[str, str], dict[str, Any]] = {}
    for path in _source_files(root, ".S"):
        relative = path.relative_to(root).as_posix()
        if relative == "src/base/pspimport.s":
            continue
        source = _read_bounded(path)
        if "IMPORT_FUNC" not in source and "IMPORT_START" not in source:
            continue
        parsed, file_libraries = parse_import_assembly(source, relative)
        for library, flags in file_libraries.items():
            previous = libraries.get(library)
            if previous is not None and previous != flags:
                raise SyncError(
                    f"library {library!r} has conflicting flags {previous} and {flags}"
                )
            libraries[library] = flags
        for entry in parsed:
            key = (entry["library"], entry["nid"])
            if key in seen:
                previous = seen[key]
                raise SyncError(
                    f"duplicate upstream import {entry['library']} {entry['nid']}: "
                    f"{previous['symbol']} at {previous['source_file']}:"
                    f"{previous['source_line']} and {entry['symbol']} at "
                    f"{entry['source_file']}:{entry['source_line']}"
                )
            seen[key] = entry
            functions.append(entry)
            if len(functions) > MAX_IMPORTS:
                raise SyncError(f"upstream import count exceeds cap {MAX_IMPORTS}")
    return (
        sorted(
            functions,
            key=lambda item: (item["library"], item["nid"], item["symbol"]),
        ),
        dict(sorted(libraries.items())),
    )


def _blank_preprocessor(source: str) -> str:
    lines = source.splitlines(keepends=True)
    output = []
    for line in lines:
        if line.lstrip().startswith("#"):
            body = line.rstrip("\r\n")
            output.append(" " * len(body) + line[len(body) :])
        else:
            output.append(line)
    return "".join(output)


def _split_parameters(raw: str) -> list[str]:
    text = raw.strip()
    if not text or text == "void":
        return []
    parts: list[str] = []
    start = 0
    paren = bracket = 0
    for index, char in enumerate(text):
        if char == "(":
            paren += 1
        elif char == ")":
            paren -= 1
            if paren < 0:
                raise SyncError("prototype parameter list has an unmatched ')'")
        elif char == "[":
            bracket += 1
        elif char == "]":
            bracket -= 1
            if bracket < 0:
                raise SyncError("prototype parameter list has an unmatched ']'")
        elif char == "," and paren == 0 and bracket == 0:
            parts.append(" ".join(text[start:index].split()))
            start = index + 1
    if paren or bracket:
        raise SyncError("prototype parameter list has unbalanced delimiters")
    parts.append(" ".join(text[start:].split()))
    if any(not part for part in parts):
        raise SyncError("prototype parameter list contains an empty parameter")
    return parts


def parse_prototypes(
    source: str,
    symbols: set[str],
    source_file: str,
) -> list[dict[str, Any]]:
    """Extract declarations only for imported symbols using a narrow scanner."""

    clean = _blank_preprocessor(_strip_comments(source))
    results: list[dict[str, Any]] = []
    for symbol in sorted(symbols):
        if not IDENT_RE.fullmatch(symbol):
            raise SyncError(f"invalid symbol requested: {symbol!r}")
        pattern = re.compile(rf"\b{re.escape(symbol)}\s*\(")
        for match in pattern.finditer(clean):
            start = max(
                clean.rfind(";", 0, match.start()),
                clean.rfind("{", 0, match.start()),
                clean.rfind("}", 0, match.start()),
            ) + 1
            if match.start() - start > MAX_DECLARATION_BYTES:
                continue
            open_paren = clean.find("(", match.start())
            depth = 0
            close_paren = -1
            index = open_paren
            while index < len(clean) and index - start <= MAX_DECLARATION_BYTES:
                char = clean[index]
                if char == "(":
                    depth += 1
                elif char == ")":
                    depth -= 1
                    if depth == 0:
                        close_paren = index
                        break
                    if depth < 0:
                        break
                index += 1
            if close_paren < 0:
                continue
            semi = clean.find(";", close_paren + 1, close_paren + 512)
            brace = clean.find("{", close_paren + 1, close_paren + 512)
            if semi < 0 or (brace >= 0 and brace < semi):
                continue
            prefix = clean[start : match.start()].strip()
            if not prefix or "=" in prefix or prefix.startswith("typedef"):
                continue
            return_type = " ".join(prefix.split())
            first_token = return_type.split(None, 1)[0]
            if first_token in {
                "return",
                "if",
                "while",
                "for",
                "switch",
                "case",
                "sizeof",
            }:
                continue
            if not RETURN_TYPE_RE.fullmatch(return_type):
                continue
            if return_type.startswith("extern "):
                return_type = return_type[len("extern ") :].strip()
            parameters = _split_parameters(clean[open_paren + 1 : close_paren])
            declaration = " ".join(clean[start : semi + 1].split())
            results.append(
                {
                    "symbol": symbol,
                    "return_type": return_type,
                    "parameters": parameters,
                    "declaration": declaration,
                    "source_file": source_file,
                    "source_line": source.count("\n", 0, match.start()) + 1,
                }
            )
    unique = {
        (item["symbol"], item["declaration"]): item for item in results
    }
    return sorted(
        unique.values(),
        key=lambda item: (
            item["symbol"],
            item["source_file"],
            item["source_line"],
        ),
    )


def scan_prototypes(
    root: Path, symbols: set[str]
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    prototypes: list[dict[str, Any]] = []
    by_symbol: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for path in _source_files(root, ".h"):
        relative = path.relative_to(root).as_posix()
        source = _read_bounded(path)
        present = symbols.intersection(IDENT_TOKEN_RE.findall(source))
        if not present:
            continue
        for item in parse_prototypes(source, present, relative):
            by_symbol[item["symbol"]].append(item)
            prototypes.append(item)
            if len(prototypes) > MAX_PROTOTYPES:
                raise SyncError(f"prototype count exceeds cap {MAX_PROTOTYPES}")
    conflicts = []
    for symbol, declarations in sorted(by_symbol.items()):
        signatures = {
            (item["return_type"], tuple(item["parameters"]))
            for item in declarations
        }
        if len(signatures) > 1:
            conflicts.append(
                {
                    "kind": "conflicting_upstream_prototypes",
                    "symbol": symbol,
                    "declarations": declarations,
                }
            )
    return (
        sorted(
            prototypes,
            key=lambda item: (
                item["symbol"],
                item["source_file"],
                item["source_line"],
            ),
        ),
        conflicts,
    )


def _run_git(root: Path, args: list[str]) -> subprocess.CompletedProcess[bytes]:
    try:
        completed = subprocess.run(
            ["git", "-C", str(root), *args],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=GIT_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise SyncError(
            f"git identity check failed: {type(exc).__name__}"
        ) from exc
    if (
        len(completed.stdout) > GIT_OUTPUT_BYTES
        or len(completed.stderr) > GIT_OUTPUT_BYTES
    ):
        raise SyncError("git identity output exceeded the bounded capture")
    return completed


def verify_source_identity(
    root: Path,
    expected_commit: str,
    *,
    asserted_commit: str | None = None,
    allow_unverified_source: bool = False,
    allow_dirty: bool = False,
    git_runner: Callable[
        [Path, list[str]], subprocess.CompletedProcess[bytes]
    ] = _run_git,
) -> dict[str, Any]:
    head = git_runner(root, ["rev-parse", "HEAD"])
    if head.returncode == 0:
        try:
            actual = head.stdout.decode("ascii", errors="strict").strip()
        except UnicodeDecodeError as exc:
            raise SyncError("git rev-parse returned non-ASCII output") from exc
        if actual != expected_commit:
            raise SyncError(
                f"PSPSDK checkout HEAD {actual} does not match lock {expected_commit}"
            )
        status = git_runner(
            root, ["status", "--porcelain=v1", "--untracked-files=all"]
        )
        if status.returncode != 0:
            raise SyncError("git status failed")
        dirty_lines = [
            line
            for line in status.stdout.decode(
                "utf-8", errors="replace"
            ).splitlines()
            if line
        ]
        if dirty_lines and not allow_dirty:
            raise SyncError(
                f"PSPSDK checkout has {len(dirty_lines)} tracked/untracked "
                "modification(s); use a clean pinned checkout or pass --allow-dirty"
            )
        return {
            "proof": "git-head",
            "commit": actual,
            "tracked_dirty": bool(dirty_lines),
            "tracked_dirty_count": len(dirty_lines),
        }
    if not allow_unverified_source:
        raise SyncError(
            "PSPSDK source is not a readable Git checkout; use a clean checkout, "
            "or explicitly pass --source-commit with --allow-unverified-source "
            "for a weaker archive/synthetic run"
        )
    if asserted_commit != expected_commit:
        raise SyncError(
            f"asserted source commit {asserted_commit!r} does not match lock "
            f"{expected_commit}"
        )
    return {
        "proof": "caller-asserted",
        "commit": expected_commit,
        "tracked_dirty": None,
        "tracked_dirty_count": None,
    }


def build_upstream_manifest(
    root: Path,
    expected_commit: str,
    identity: dict[str, Any],
) -> dict[str, Any]:
    imports, library_flags = scan_imports(root)
    symbols = {entry["symbol"] for entry in imports}
    prototypes, conflicts = scan_prototypes(root, symbols)
    prototypes_by_symbol: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for prototype in prototypes:
        prototypes_by_symbol[prototype["symbol"]].append(prototype)

    library_map: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for entry in imports:
        enriched = dict(entry)
        enriched["prototypes"] = prototypes_by_symbol.get(entry["symbol"], [])
        library_map[entry["library"]].append(enriched)

    libraries = []
    for name in sorted(set(library_flags) | set(library_map)):
        libraries.append(
            {
                "name": name,
                "flags": library_flags.get(name),
                "functions": sorted(
                    library_map.get(name, []),
                    key=lambda item: (item["nid"], item["symbol"]),
                ),
            }
        )
    return {
        "schema": SCHEMA,
        "upstream": {
            "component": "pspsdk",
            "repository": pspdev_lock.EXPECTED_COMPONENTS["pspsdk"],
            "commit": expected_commit,
            "identity": identity,
        },
        "limits": {
            "source_files": MAX_SOURCE_FILES,
            "source_file_bytes": MAX_SOURCE_FILE_BYTES,
            "imports": MAX_IMPORTS,
            "prototypes": MAX_PROTOTYPES,
            "declaration_bytes": MAX_DECLARATION_BYTES,
        },
        "libraries": libraries,
        "prototype_conflicts": conflicts,
        "statistics": {
            "libraries": len(libraries),
            "imports": len(imports),
            "symbols": len(symbols),
            "symbols_with_prototypes": len(prototypes_by_symbol),
            "prototype_declarations": len(prototypes),
            "prototype_conflicts": len(conflicts),
        },
    }
