# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Seeded, deterministic mutation fuzzing over the public offline parsers.

Every mutated input must either be accepted or raise ``ValueError`` -- any
other exception (``struct.error``, ``IndexError``, ``OverflowError``,
``RecursionError``, ``MemoryError``, ``SystemExit``, ...) is a parser escape
and fails the suite with the offending bytes attached so it can be persisted
as a regression fixture. Iteration counts and input sizes are bounded so the
suite stays fast and deterministic; inputs are capped at 4 KiB, so forged
header fields cannot request unbounded host allocation or runtime.

Streams:
- random bytes: exercises the envelope fail-closed rejections from scratch;
- mutations of a valid synthetic ELF: reaches the relocation and import-table
  parser internals, not just the envelope.
"""

from __future__ import annotations

import ast
import copy
import json
import random
import re
import shutil
import struct
import tempfile
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import analyze  # noqa: E402
import elf_bounds  # noqa: E402
import imports  # noqa: E402
import prxload  # noqa: E402
import title_manifest  # noqa: E402
from nk_core.iso_inspect import parse_param_sfo, IsoInspectionError  # noqa: E402
from test_import_name_safety import build_synthetic_import_prx  # noqa: E402
from test_iso_parity import build_param_sfo  # noqa: E402
from test_xb_probe import _make_archive, XBCompression  # noqa: E402
from xb_probe import XBArchiveReader, XBProbeError  # noqa: E402


SEED_BASE = 0x08804000


def valid_seed_prx() -> bytes:
    """A deterministic synthetic PRX with module-info and one import stub.

    The shared builder is deliberately public/synthetic and gives the fuzz
    stream a seed that passes the PRX loader, analyzer, and import parser.
    """
    blob, _ = build_synthetic_import_prx(b"sceDisplay", SEED_BASE)
    return blob


def _run_stage(stage: str, action, data: bytes):
    """Run one parser stage, allowing only its documented malformed-input error.

    A library ``SystemExit`` is converted to an assertion failure with the
    stage and input summary. Other exception types are deliberately not caught:
    unittest must expose them as parser escapes rather than hiding them.
    """
    try:
        return "accepted", action()
    except ValueError as exc:
        return "rejected", str(exc)
    except SystemExit as exc:
        raise AssertionError(
            f"{stage} raised SystemExit for {describe(data)}: {exc!r}"
        ) from exc


def exercise(data: bytes, base: int, path: Path | None = None) -> dict[str, object]:
    """Run the parser pipeline and report the stage reached by the input.

    A rejected stage stops the dependent stages and marks them ``not_reached``;
    it is not reported as if every parser had consumed the same bytes.
    """
    if path is not None and not isinstance(data, (bytes, bytearray, memoryview)):
        path.write_bytes(data)
    report = {
        "prx": "not_reached",
        "analyze": "not_reached",
        "imports": "not_reached",
        "module_info": False,
        "import_count": None,
    }

    def load_prx():
        prx = prxload.Prx(data, base)
        prx.relocate()
        return prx

    status, _ = _run_stage("prx", load_prx, data)
    report["prx"] = status
    if status == "rejected":
        return report

    status, elf = _run_stage(
        "analyze", lambda: analyze.Elf(data, base), data
    )
    report["analyze"] = status
    if status == "rejected":
        return report

    report["module_info"] = elf.sec(".rodata.sceModuleInfo") is not None
    status, parsed = _run_stage(
        "imports", lambda: imports.parse_imports(elf), data
    )
    report["imports"] = status
    if status == "accepted":
        report["import_count"] = len(parsed)
    return report


def _mutate_bytes(data: bytes, rng: random.Random, max_cap: int = 4096) -> bytes:
    d = bytearray(data)
    for _ in range(rng.randint(1, 8)):
        op = rng.randrange(6)
        if op == 0 and d:
            d[rng.randrange(len(d))] ^= (1 << rng.randrange(8))
        elif op == 1 and len(d) < max_cap:
            d.insert(rng.randrange(len(d) + 1), rng.randrange(256))
        elif op == 2 and len(d) > 4:
            if rng.randrange(4) == 0:
                del d[rng.randrange(len(d)):]
            else:
                del d[rng.randrange(len(d))]
        elif op == 3 and d:
            d[rng.randrange(len(d))] = rng.randrange(256)
        elif op == 4 and len(d) >= 4:
            pos = rng.randrange(len(d) - 3)
            val = rng.choice([0, 1, 2, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, len(d)])
            struct.pack_into("<I", d, pos, val)
        elif op == 5 and len(d) >= 2:
            pos = rng.randrange(len(d) - 1)
            val = rng.choice([0, 1, 0x7FFF, 0x8000, 0xFFFF])
            struct.pack_into("<H", d, pos, val)
    return bytes(d)


def describe(data: bytes) -> str:
    import hashlib

    return f"len={len(data)} sha256={hashlib.sha256(data).hexdigest()[:16]} bytes={data.hex()}"


class TestParseFuzz(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="parsefuzz_")
        cls.path = Path(cls.tmp) / "fuzz.elf"

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_random_blob_stream_reports_reached_stages(self) -> None:
        rng = random.Random(0x5EE7)
        for _ in range(1500):
            n = rng.randint(1, 2048)
            data = rng.randbytes(n)
            base = rng.choice([0, 0x08000000, 0x08004000, 0xFFFFFFFF])
            report = exercise(data, base, self.path)
            if report["prx"] == "rejected":
                self.assertEqual(report["analyze"], "not_reached")
                self.assertEqual(report["imports"], "not_reached")
            elif report["analyze"] == "rejected":
                self.assertEqual(report["imports"], "not_reached")

    def test_seed_mutation_stream_reports_reached_stages(self) -> None:
        rng = random.Random(0x51F7)
        seed = bytearray(valid_seed_prx())
        reached_analyze = 0
        reached_imports = 0
        for _ in range(1500):
            data = bytearray(seed)
            ops = rng.randint(1, 12)
            for _ in range(ops):
                kind = rng.randrange(5)
                if kind == 0 and data:  # bit flip
                    pos = rng.randrange(len(data))
                    data[pos] ^= 1 << rng.randrange(8)
                elif kind == 1 and len(data) < 4096:  # byte insert
                    pos = rng.randrange(len(data) + 1)
                    data.insert(pos, rng.randrange(256))
                elif kind == 2 and data:  # byte delete
                    del data[rng.randrange(len(data))]
                elif kind == 3 and data:  # byte run overwrite
                    pos = rng.randrange(len(data))
                    data[pos] = rng.randrange(256)
                elif kind == 4 and data:  # header word scribble
                    pos = rng.randrange(len(data) - 3)
                    struct.pack_into("<I", data, pos, rng.randrange(0x100000000))
            report = exercise(bytes(data), rng.choice([0, SEED_BASE]), self.path)
            reached_analyze += report["analyze"] != "not_reached"
            reached_imports += report["imports"] != "not_reached"
            if report["prx"] == "rejected":
                self.assertEqual(report["analyze"], "not_reached")
                self.assertEqual(report["imports"], "not_reached")
            elif report["analyze"] == "rejected":
                self.assertEqual(report["imports"], "not_reached")
        self.assertGreater(reached_analyze, 0, "mutations never reached analyze.Elf")
        self.assertGreater(reached_imports, 0, "mutations never reached parse_imports")

    def test_valid_seed_is_accepted(self) -> None:
        report = exercise(valid_seed_prx(), SEED_BASE, self.path)
        self.assertEqual(report["prx"], "accepted")
        self.assertEqual(report["analyze"], "accepted")
        self.assertTrue(report["module_info"])
        self.assertEqual(report["imports"], "accepted")
        self.assertEqual(report["import_count"], 1)

    def test_prx_rejection_does_not_claim_downstream_stages(self) -> None:
        report = exercise(b"not an ELF", SEED_BASE, self.path)
        self.assertEqual(report["prx"], "rejected")
        self.assertEqual(report["analyze"], "not_reached")
        self.assertEqual(report["imports"], "not_reached")

    def test_systemexit_is_an_explicit_test_failure(self) -> None:
        original = imports.parse_imports

        def raise_system_exit(_elf):
            raise SystemExit("library escape")

        imports.parse_imports = raise_system_exit
        try:
            with self.assertRaisesRegex(AssertionError, "imports raised SystemExit"):
                exercise(valid_seed_prx(), SEED_BASE, self.path)
        finally:
            imports.parse_imports = original

    def test_param_sfo_fuzz(self) -> None:
        rng = random.Random(0x5F0)
        seed = build_param_sfo("ULES00123", "Test Title", "1.00")
        accepted = 0
        for _ in range(500):
            if rng.random() < 0.5:
                data = _mutate_bytes(seed, rng)
            else:
                data = rng.randbytes(rng.randint(0, 1024))
            try:
                res = parse_param_sfo(data)
                self.assertIsInstance(res, dict)
                accepted += 1
            except (IsoInspectionError, ValueError):
                pass
        self.assertGreater(accepted, 0)

    def test_xb_archive_fuzz(self) -> None:
        rng = random.Random(0x8B)
        seed = _make_archive([
            ("data/file1.bin", b"hello world", XBCompression.NONE),
            ("data/file2.bin", b"synthetic content for lzs compression", XBCompression.LZS),
        ])
        accepted = 0
        for _ in range(500):
            if rng.random() < 0.5:
                data = _mutate_bytes(seed, rng)
            else:
                data = rng.randbytes(rng.randint(0, 1024))
            try:
                reader = XBArchiveReader.from_bytes(data)
                accepted += 1
                if reader.entries:
                    try:
                        reader.read_entry(reader.entries[0])
                    except (XBProbeError, ValueError):
                        pass
            except (XBProbeError, ValueError):
                pass
        self.assertGreater(accepted, 0)

    def test_elf_bounds_fuzz(self) -> None:
        rng = random.Random(0x31F)
        seed = valid_seed_prx()
        accepted = 0
        for _ in range(500):
            if rng.random() < 0.5:
                data = _mutate_bytes(seed, rng)
            else:
                data = rng.randbytes(rng.randint(0, 1024))
            try:
                res = elf_bounds.validate_elf32_envelope(data)
                self.assertIsInstance(res, dict)
                accepted += 1
            except ValueError:
                pass
        self.assertGreater(accepted, 0)

    def test_title_manifest_fuzz(self) -> None:
        rng = random.Random(0x7171)
        fixture_path = ROOT / "assets" / "titles" / "synthetic.json"
        seed_text = fixture_path.read_text(encoding="utf-8")
        accepted = 0
        for _ in range(500):
            if rng.random() < 0.5:
                mutated = _mutate_bytes(seed_text.encode("utf-8"), rng).decode("utf-8", errors="ignore")
            else:
                mutated = rng.randbytes(rng.randint(0, 1024)).decode("utf-8", errors="ignore")
            try:
                raw_obj = title_manifest.loads_manifest(mutated)
                title_manifest.validate_manifest(raw_obj)
                accepted += 1
            except (title_manifest.TitleManifestError, ValueError):
                pass
        self.assertGreater(accepted, 0)

    def test_psp_segment_sizes_fuzz(self) -> None:
        rng = random.Random(0x5E6)
        seed = bytearray(0x80)
        seed[:4] = b"~PSP"
        seed[0x27] = 2
        struct.pack_into("<I", seed, 0x38, 0x1000)
        struct.pack_into("<4I", seed, 0x54, 0x2000, 0x3000, 0, 0)
        seed_bytes = bytes(seed)
        accepted = 0
        for _ in range(500):
            if rng.random() < 0.5:
                data = _mutate_bytes(seed_bytes, rng)
            else:
                data = rng.randbytes(rng.randint(0, 256))
            try:
                sizes, bss = prxload.read_psp_segment_sizes(data, 2)
                self.assertIsInstance(sizes, list)
                self.assertIsInstance(bss, int)
                accepted += 1
            except ValueError:
                pass
        self.assertGreater(accepted, 0)


class TestParserSafetyInventory(unittest.TestCase):
    """Keep the maintained #319 inventory machine-readable and source-backed."""

    # An inventory anchor is evidence only if a test can still resolve it, so the
    # grammar is checked here rather than trusted to review. See the
    # ``anchor_grammar`` object in the inventory for the plain-language form.
    # The path must be repo-relative (it carries a directory separator): a bare
    # filename in prose ("manifest.json", "package.json") names a product
    # document, not a file in this tree, and is not an anchor.
    # Legacy numeric suffixes remain visible to the scanner so the checker can
    # reject them explicitly rather than silently skipping them.
    ANCHOR = re.compile(
        r"(?=[A-Za-z0-9_./\\-]*/)"
        r"[A-Za-z0-9_./\\-]+\.(?:c|h|py|json|yml|yaml|md)"
        r"(?::(?:\d+(?:@[A-Za-z_][A-Za-z0-9_]*| [A-Za-z_][A-Za-z0-9_]*(?:[./][A-Za-z_][A-Za-z0-9_]*)*)?"
        r"|[A-Za-z_][A-Za-z0-9_]*(?:[./][A-Za-z_][A-Za-z0-9_]*)*))?"
    )
    SYMBOL = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*")
    # A C function header is checked by splitting, not by one nested regex: a
    # repeated qualifier group such as (struct\s+\w+\s*)* backtracks
    # exponentially on hostile lines (CodeQL py/redos).
    C_HEADER_TEXT = re.compile(r"[A-Za-z_][A-Za-z0-9_ \t*]*")
    C_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
    C_DEFINE = re.compile(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\b")
    C_STATIC_CONST = re.compile(
        r"^\s*static\s+const\b.*?\b([A-Za-z_][A-Za-z0-9_]*)\s*"
        r"(?:\[[^\]]*\])?\s*(?:=|;)"
    )
    PY_DEF = re.compile(r"^(\s*)(?:async\s+)?(?:def|class)\s+([A-Za-z_][A-Za-z0-9_]*)")

    # A route-coverage claim ("no test executes it", "never reaches the reader")
    # has to say which kind of test is missing. A seeded-mutation, sanitizer or
    # fuzzer sentence is a different claim about a different campaign, and a
    # sentence about what a route does not publish is not a claim about tests.
    CAMPAIGN_CLAIM = re.compile(
        r"seeded|mutation|sanitiz|coverage-guided|coverage_guided|fuzz|campaign", re.IGNORECASE
    )
    COVERAGE_CLAIM = re.compile(
        r"(?:\bno\b|\bnot\b|\bnever\b|\bwithout\b)[^.;\"]{0,60}?"
        r"\b(?:tests?|testing|tested|untested|executed|exercises|exercised|reaches?|calls?)\b",
        re.IGNORECASE,
    )
    DIRECT_QUALIFIER = re.compile(r"\bdirect(?:ly)?\b", re.IGNORECASE)
    PUBLISH_CLAIM = re.compile(r"\bpublish", re.IGNORECASE)
    # "<test path> never reaches <symbol>" and its relatives name a reachability
    # the row then depends on, so it is checked instead of trusted. Only a claim
    # that names a symbol defined in the row's own sources is checked; a claim
    # whose object is prose ("do not reach the latter two readers") names nothing
    # the checker can resolve and is left alone.
    DOTTED_NAME = r"((?:[A-Za-z_][A-Za-z0-9_]*\.)*[A-Za-z_][A-Za-z0-9_]*)"
    UNREACHED_CLAIM = re.compile(
        rf"(?:never|not)\s+reach(?:es|ed)?\b[^.;\"]*?{DOTTED_NAME}"
        rf"|no\s+test\s+calls?\b[^.;\"]*?{DOTTED_NAME}",
        re.IGNORECASE,
    )
    PY_CALL = re.compile(r"(?:\.\s*)?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
    CALL_DEPTH = 3

    @classmethod
    def setUpClass(cls) -> None:
        cls.inventory = json.loads(
            (ROOT / "docs" / "PARSER_SAFETY_INVENTORY.json").read_text(encoding="utf-8")
        )
        cls.source_cache: dict[str, list[str]] = {}

    @classmethod
    def source_lines(cls, rel: str) -> list[str]:
        if rel not in cls.source_cache:
            cls.source_cache[rel] = (ROOT / rel).read_text(
                encoding="utf-8", errors="replace"
            ).splitlines()
        return cls.source_cache[rel]

    @classmethod
    def c_function_header(cls, line: str) -> tuple[str, int] | None:
        """Return (name, index after its opening parenthesis) for a C function header.

        The header is a return type made of identifiers and ``*`` starting in
        column 0, then the function name and ``(``. Calls inside bodies are
        indented and never match.
        """
        paren = line.find("(")
        if paren <= 0:
            return None
        head = line[:paren]
        if not cls.C_HEADER_TEXT.fullmatch(head):
            return None
        tokens = [token for token in re.split(r"[ \t*]+", head) if token]
        if len(tokens) < 2 or not all(cls.C_IDENTIFIER.fullmatch(token) for token in tokens):
            return None
        if not head.rstrip().endswith(tokens[-1]):
            return None
        return tokens[-1], paren + 1

    @classmethod
    def definition_line(cls, line: str) -> str | None:
        """Return the name a line *defines*, or None when it defines nothing.

        C function prototypes are declarations. Macros, static const objects,
        and module-level Python assignments are named definitions too.
        """
        match = cls.PY_DEF.match(line)
        if match is not None:
            return match.group(2)
        match = cls.C_DEFINE.match(line)
        if match is not None:
            return match.group(1)
        match = cls.C_STATIC_CONST.match(line)
        if match is not None:
            return match.group(1)
        if line.rstrip().endswith(";"):
            return None
        header = cls.c_function_header(line)
        return header[0] if header is not None else None

    @classmethod
    def _c_function_has_body(cls, lines: list[str], line_index: int, body_start: int) -> bool:
        """Distinguish a C function definition from a possibly multiline prototype."""
        header = "\n".join(lines[line_index:])
        depth = 1  # body_start is just past the function's opening parenthesis.
        for offset, char in enumerate(header[body_start:], body_start):
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    delimiter = re.search(r"[;{}]", header[offset + 1:])
                    return delimiter is not None and delimiter.group(0) == "{"
        return False

    @classmethod
    def definition_map(cls, rel: str) -> dict[str, list[int]]:
        """Map each supported symbol spelling to its definition lines."""
        lines = cls.source_lines(rel)
        found: dict[str, list[int]] = {}

        def add(name: str, line: int) -> None:
            found.setdefault(name, []).append(line)

        if rel.endswith(".py"):
            tree = ast.parse("\n".join(lines), filename=rel)

            def assigned_names(target: ast.expr) -> list[str]:
                if isinstance(target, ast.Name):
                    return [target.id]
                if isinstance(target, (ast.Tuple, ast.List)):
                    return [name for item in target.elts for name in assigned_names(item)]
                return []

            def visit_scope(body: list[ast.stmt], prefix: str = "", module: bool = False) -> None:
                for node in body:
                    if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                        if module:
                            add(node.name, node.lineno)
                        elif prefix:
                            add(f"{prefix}.{node.name}", node.lineno)
                    elif isinstance(node, ast.ClassDef):
                        qualified = f"{prefix}.{node.name}" if prefix else node.name
                        add(qualified, node.lineno)
                        visit_scope(node.body, qualified)
                    elif module and isinstance(node, (ast.Assign, ast.AnnAssign)):
                        targets = node.targets if isinstance(node, ast.Assign) else [node.target]
                        for target in targets:
                            for name in assigned_names(target):
                                add(name, node.lineno)

            visit_scope(tree.body, module=True)
            return found

        for index, line in enumerate(lines):
            function = cls.c_function_header(line)
            if function is not None:
                if cls._c_function_has_body(lines, index, function[1]):
                    add(function[0], index + 1)
                continue
            name = cls.definition_line(line)
            if name is not None:
                add(name, index + 1)
        return found

    @classmethod
    def enclosing_function(cls, rel: str) -> dict[int, str]:
        """Map each line of *rel* to the name of the function that contains it."""
        cache = getattr(cls, "_extent_cache", None)
        if cache is None:
            cache = cls._extent_cache = {}
        if rel in cache:
            return cache[rel]
        lines = cls.source_lines(rel)
        starts: list[tuple[int, int, str]] = []
        for number, line in enumerate(lines, 1):
            if rel.endswith(".py"):
                match = cls.PY_DEF.match(line)
                if match:
                    starts.append((number, len(match.group(1)), match.group(2)))
                    continue
            name = cls.definition_line(line)
            if name is not None:
                starts.append((number, 0, name))
        owner: dict[int, str] = {}
        for index, (start, indent, name) in enumerate(starts):
            end = len(lines)
            for following, following_indent, _ in starts[index + 1:]:
                if following_indent <= indent:
                    end = following - 1
                    break
            for number in range(start, min(end, len(lines)) + 1):
                owner[number] = name
        cache[rel] = owner
        return owner

    @classmethod
    def iter_strings(cls, node, field: str = ""):
        """Yield ``(field, text)`` for every string anywhere under *node*."""
        if isinstance(node, str):
            yield field, node
        elif isinstance(node, list):
            for index, value in enumerate(node):
                child = f"{field}[{index}]" if field else f"[{index}]"
                yield from cls.iter_strings(value, child)
        elif isinstance(node, dict):
            for key, value in node.items():
                child = f"{field}.{key}" if field else key
                yield from cls.iter_strings(value, child)

    @classmethod
    def scan_anchors(cls, node, field: str = ""):
        """Yield ``(field, anchor)`` for every anchor in any string under *node*.

        The whole surface is walked, not a hand-picked list of fields, so a new
        narrative field cannot carry an unchecked symbol reference.
        """
        for text_field, text in cls.iter_strings(node, field):
            for match in cls.ANCHOR.finditer(text):
                yield text_field, match.group(0)

    def row(self, surface_id: str) -> dict:
        for surface in self.inventory["surfaces"]:
            if surface["id"] == surface_id:
                return surface
        self.fail(f"no inventory row for {surface_id}")

    def check_anchor(self, ref: str, where: str) -> None:
        """Verify one inventory anchor against current source.

        A symbol anchor must name exactly one definition in its file. Line-number
        forms are rejected so an insertion above a definition cannot rot the
        inventory without changing what it names.
        """
        rel, _, suffix = ref.partition(":")
        self.assertTrue((ROOT / rel).is_file(), f"{where}: missing file {rel}")
        if not suffix:
            return
        self.assertFalse(
            suffix[0].isdigit(),
            f"{where}: line-number anchor {ref} is forbidden; use path:symbol",
        )
        self.assertIsNotNone(
            self.SYMBOL.fullmatch(suffix), f"{where}: invalid symbol anchor {ref}"
        )
        definitions = self.definition_map(rel).get(suffix, [])
        self.assertEqual(
            len(definitions),
            1,
            f"{where}: {ref} must resolve to exactly one definition, found {len(definitions)}",
        )

    def assert_inventory_anchors(self, inventory: dict) -> None:
        checked = 0
        scanned: set[str] = set()
        preamble = {k: v for k, v in inventory.items() if k != "surfaces"}
        for node, field in [(preamble, "inventory")] + [
            (surface, "surface") for surface in inventory["surfaces"]
        ]:
            for text_field, anchor in self.scan_anchors(node, field):
                with self.subTest(field=text_field, anchor=anchor):
                    self.check_anchor(anchor, text_field)
                checked += 1
                scanned.add(text_field)
        # Fields the first version of this scan skipped; a narrowing of the walk
        # would drop them from `scanned` and fail here rather than silently.
        for prefix in (
            "surface.campaigns.deterministic_regression.runner",
            "surface.campaigns.seeded_mutations.campaign",
            "surface.resource_gaps",
            "surface.max_input",
            "inventory.campaign_facts",
            "inventory.issue_acceptance_remaining",
        ):
            self.assertTrue(
                any(field.startswith(prefix) for field in scanned), f"no anchor scanned in {prefix}"
            )
        self.assertGreater(checked, 300, "inventory lost most of its anchors")

    def test_c_function_header_is_linear_and_keeps_definition_shapes(self) -> None:
        header = self.c_function_header
        self.assertEqual(header("static int foo(void) {"), ("foo", 15))
        self.assertEqual(header("static const struct nk_x *bar(int a)")[0], "bar")
        self.assertEqual(header("unsigned int baz (int a)")[0], "baz")
        self.assertEqual(header("extern bool nk_iso_reader_open(NkIso *iso,")[0],
                         "nk_iso_reader_open")
        self.assertIsNone(header("    call_inside_body(x);"))
        self.assertIsNone(header("x = compute(1);"))
        self.assertIsNone(header("int *(y);"))
        # Hostile shapes that made the former nested-quantifier regex backtrack
        # exponentially (CodeQL py/redos) finish at once.
        for hostile in ("struct " + "0signed struct " * 5000 + "!",
                        "struct 0" + " struct 0" * 5000 + "!"):
            self.assertIsNone(header(hostile))

    def test_every_inventory_row_has_live_sources_tests_and_campaign_status(self) -> None:
        inventory = self.inventory
        self.assertEqual(inventory["schema_version"], 4)
        self.assertEqual(inventory["issue"], 319)
        self.assertEqual(
            set(inventory["status_definition"]), set(inventory["status_values"])
        )
        self.assertIn("anchor_grammar", inventory)

        surfaces = inventory["surfaces"]
        ids = [surface["id"] for surface in surfaces]
        self.assertEqual(len(ids), len(set(ids)))
        required = {
            "iso9660-pvd-directory-c", "param-sfo-c", "encrypted-psp-kirk",
            "elf32-mips-classifier-c", "prx-loader-c", "prx-relocation-a-c",
            "prx-relocation-b-c", "title-manifest-c", "runtime-package-json-c",
            "private-package-cache-json", "xb-provider-c", "library-json-c",
            "psmf-header-c", "mpeg-ps-pes-c", "h264-au-framing-c",
            "windows-mf-demux", "atrac-frame-framing-c", "pgf-public-reader-c",
            "python-iso-sfo-inspector", "python-elf-prx-import",
            "python-title-manifest", "python-xb-archive-extractor",
            "python-gim-image", "python-library-json",
            "atrac3plus-bitstream-decoder",
            "nk-json-ast-c", "nk-input-profile-json", "nk-font-cache-manifest",
            "python-font-cache-manifest", "player-package-builder-json",
            "player-settings-json", "python-flight-bundle-json",
            "python-ppm-visual-tools", "python-tooling-json-readers",
        }
        self.assertTrue(required.issubset(set(ids)), sorted(required - set(ids)))

        for surface in surfaces:
            with self.subTest(surface=surface["id"]):
                self.assertIn(surface["status"], inventory["status_values"])
                self.assertTrue(surface["source_paths"])
                self.assertTrue(surface["test_paths"])
                self.assertIsInstance(surface["malformed_input_coverage"], list)
                self.assertTrue(surface["malformed_input_coverage"])
                self.assertIsInstance(surface["resource_gaps"], list)
                for ref in surface["source_paths"] + surface["test_paths"]:
                    self.assertTrue((ROOT / ref).is_file(), ref)

                arithmetic = surface["arithmetic"]
                self.assertIsInstance(arithmetic["safe"], str)
                self.assertIn(arithmetic["safe"], inventory["arithmetic_status_values"])
                self.assertTrue(arithmetic["evidence"])
                self.assertTrue(surface["resource_gaps"])
                if arithmetic["safe"] != "SAFE":
                    self.assertNotEqual(
                        surface["status"], "COVERED",
                        f"{surface['id']} claims COVERED with {arithmetic['safe']} arithmetic",
                    )

                campaigns = surface["campaigns"]
                self.assertEqual(
                    set(campaigns),
                    {
                        "deterministic_regression", "seeded_mutations", "sanitizer",
                        "coverage_guided_fuzzer", "overflow_oob_mutation_kill",
                    },
                )
                self.assertIsInstance(campaigns["coverage_guided_fuzzer"], str)
                self.assertTrue(campaigns["coverage_guided_fuzzer"])
                self.assertIsInstance(campaigns["overflow_oob_mutation_kill"], str)
                self.assertTrue(campaigns["overflow_oob_mutation_kill"])
                sanitizer = campaigns["sanitizer"]
                self.assertIn("status", sanitizer)
                self.assertIn("ci", sanitizer)
                if sanitizer["ci"]:
                    self.assertTrue(sanitizer["status"].startswith("CI_"))

        self.assertTrue(inventory["campaign_facts"]["overflow_oob_mutation_kill"])

    def test_every_anchored_symbol_resolves_in_current_source(self) -> None:
        """An anchor is evidence only while it still points at the code it names.

        Every string in every surface and in the document preamble is walked, not
        a fixed list of fields: a symbol claim in a runner sentence, a
        resource gap, or a campaign note is evidence too, and a scan that skipped
        those fields would let them rot unnoticed.
        """
        self.assert_inventory_anchors(self.inventory)

    def assert_inventory_copy_anchor_fails(self, ref: str, message: str) -> None:
        """Check a deliberately broken anchor planted only in a copied inventory."""
        inventory = copy.deepcopy(self.inventory)
        row = inventory["surfaces"][0]
        row["entry"] += f"; {ref}"
        self.assertIn(ref, [anchor for _, anchor in self.scan_anchors(row)])
        with self.assertRaisesRegex(AssertionError, message):
            self.check_anchor(ref, "synthetic.inventory-copy")

    def test_nonexistent_symbol_in_inventory_copy_is_rejected(self) -> None:
        self.assert_inventory_copy_anchor_fails(
            "src/core/nk_iso.c:parser_symbol_that_does_not_exist",
            "exactly one definition, found 0",
        )

    def test_prototype_only_symbol_in_inventory_copy_is_rejected(self) -> None:
        self.assert_inventory_copy_anchor_fails(
            "src/core/nk_json.h:nk_json_validate_utf8",
            "exactly one definition, found 0",
        )

    def test_line_number_anchor_in_inventory_copy_is_rejected(self) -> None:
        self.assert_inventory_copy_anchor_fails(
            "src/core/nk_iso.c:114 nk_iso_reader_open", "line-number anchor.*forbidden"
        )

    def test_renamed_real_function_anchor_in_inventory_copy_is_rejected(self) -> None:
        """Renaming a real symbol in the copied inventory loses its definition."""
        inventory = copy.deepcopy(self.inventory)
        iso_row = next(row for row in inventory["surfaces"] if row["id"] == "iso9660-pvd-directory-c")
        old = "src/core/nk_iso.c:nk_iso_reader_open"
        renamed = "src/core/nk_iso.c:nk_iso_reader_open_renamed"
        self.assertIn(old, iso_row["entry"])
        iso_row["entry"] = iso_row["entry"].replace(old, renamed, 1)
        self.assertIn(renamed, [anchor for _, anchor in self.scan_anchors(iso_row)])
        with self.assertRaisesRegex(AssertionError, "exactly one definition, found 0"):
            self.check_anchor(renamed, "synthetic.renamed-function")

    def test_anchor_scan_reaches_fields_the_original_four_field_walk_skipped(self) -> None:
        """The walk covers every field, including the ones it used to skip.

        A deliberately unresolvable anchor is planted in a runner sentence, a
        resource gap, and a campaign note: the original four-field walk
        (entry, harness, arithmetic evidence, campaign tests) never looked at any
        of them, so a scan that still skipped them would report nothing here.
        """
        planted = {
            "id": "synthetic-scan-coverage",
            "entry": "tools/parse_fuzz_probe_missing.c:probe_missing",
            "harness": "nothing to see",
            "arithmetic": {"safe": "SAFE", "evidence": ["tools/parse_fuzz_probe_missing.c:probe_missing"]},
            "campaigns": {
                "deterministic_regression": {
                    "status": "IN_CI",
                    "tests": [],
                    "runner": "reached only through tools/parse_fuzz_probe_missing.c:probe_missing",
                },
                "seeded_mutations": {
                    "status": "NOT_AVAILABLE",
                    "tests": [],
                    "campaign": "no campaign for tools/parse_fuzz_probe_missing.c:probe_missing",
                },
            },
            "resource_gaps": ["uncovered: tools/parse_fuzz_probe_missing.c:probe_missing"],
            "max_input": "nothing; see tools/parse_fuzz_probe_missing.c:probe_missing",
        }
        scanned = dict(self.scan_anchors(planted, "surface"))
        for field in (
            "surface.campaigns.deterministic_regression.runner",
            "surface.campaigns.seeded_mutations.campaign",
            "surface.resource_gaps[0]",
            "surface.max_input",
        ):
            self.assertIn(field, scanned)
            with self.subTest(field=field):
                with self.assertRaises(AssertionError):
                    self.check_anchor(scanned[field], f"synthetic.{field}")

    def test_definition_map_accepts_explicit_symbol_definitions(self) -> None:
        """Macros, static const objects and Python constants/methods are anchors."""
        c_path = "src/core/nk_json.c"
        py_path = "tools/test_parse_fuzz.py"
        saved = {rel: self.source_cache.get(rel) for rel in (c_path, py_path)}
        try:
            self.source_cache[c_path] = [
                "#define JSON_NODE_LIMIT 64",
                "static const size_t JSON_DEFAULT_LIMIT = 32;",
                "int prototype_only(int value);",
                "int implemented(int value) { return value; }",
            ]
            c_definitions = self.definition_map(c_path)
            self.assertEqual(c_definitions["JSON_NODE_LIMIT"], [1])
            self.assertEqual(c_definitions["JSON_DEFAULT_LIMIT"], [2])
            self.assertNotIn("prototype_only", c_definitions)
            self.assertEqual(c_definitions["implemented"], [4])

            self.source_cache[py_path] = [
                "JSON_NODE_LIMIT = 64",
                "class JsonReader:",
                "    def parse(self):",
                "        return JSON_NODE_LIMIT",
            ]
            py_definitions = self.definition_map(py_path)
            self.assertEqual(py_definitions["JSON_NODE_LIMIT"], [1])
            self.assertEqual(py_definitions["JsonReader.parse"], [3])
        finally:
            for rel, lines in saved.items():
                if lines is None:
                    self.source_cache.pop(rel, None)
                else:
                    self.source_cache[rel] = lines

    def test_ambiguous_duplicate_definition_in_inventory_copy_is_rejected(self) -> None:
        rel = "src/core/nk_json.c"
        previous = self.source_cache.get(rel)
        self.source_cache[rel] = [
            "int duplicated_parser(void) { return 0; }",
            "int duplicated_parser(void) { return 1; }",
        ]
        try:
            self.assert_inventory_copy_anchor_fails(
                f"{rel}:duplicated_parser", "exactly one definition, found 2"
            )
        finally:
            if previous is None:
                self.source_cache.pop(rel, None)
            else:
                self.source_cache[rel] = previous

    def test_schema_changes_are_versioned_and_documented(self) -> None:
        """Arithmetic status and source-anchor grammar changes have schema notes."""
        inventory = self.inventory
        notes = {note["version"]: note["change"] for note in inventory["schema_notes"]}
        self.assertIn(2, notes)
        self.assertIn(3, notes)
        self.assertIn(4, notes)
        self.assertIn("boolean", notes[2])
        self.assertIn("boolean", notes[3])
        self.assertIn("arithmetic_status_values", notes[3])
        self.assertIn("exactly one definition", notes[4])
        self.assertIn("Numeric line anchors were removed", notes[4])
        self.assertIn("exactly one definition", inventory["anchor_grammar"]["path:symbol"])
        self.assertIn("rejected", inventory["anchor_grammar"]["line-number anchors"])
        for surface in inventory["surfaces"]:
            with self.subTest(surface=surface["id"]):
                self.assertNotIsInstance(surface["arithmetic"]["safe"], bool)

    def test_route_coverage_claims_say_which_kind_of_test_is_missing(self) -> None:
        """A row may only call a route untested when it names the missing test kind.

        Every reader in this inventory is at least reached indirectly by a listed
        test, so an unqualified "no test executes it" / "never reaches" sentence
        is a false claim. "No direct test" is the honest form and stays allowed.
        """
        for surface in self.inventory["surfaces"]:
            for field, text in self.iter_strings(surface, "surface"):
                for match in self.COVERAGE_CLAIM.finditer(text):
                    claim = match.group(0)
                    if self.CAMPAIGN_CLAIM.search(claim):
                        continue
                    if self.PUBLISH_CLAIM.search(claim):
                        continue
                    with self.subTest(surface=surface["id"], field=field, claim=claim):
                        self.assertRegex(
                            claim,
                            self.DIRECT_QUALIFIER,
                            f"{surface['id']}.{field}: {claim.strip()!r} does not say whether "
                            "a direct test is missing, so it reads as no coverage at all",
                        )

    def assert_no_unreached_claims(self, inventory: dict) -> None:
        for surface in inventory["surfaces"]:
            py = [
                rel
                for rel in surface["source_paths"] + surface["test_paths"]
                if rel.endswith(".py")
            ]
            if not py:
                continue
            edges = {rel: self.python_call_edges(rel) for rel in py}
            defined = {name for rel in py for name in set(self.enclosing_function(rel).values())}
            for field, text in self.iter_strings(surface, "surface"):
                for match in self.UNREACHED_CLAIM.finditer(text):
                    named = next(group for group in match.groups() if group)
                    symbol = named.rsplit(".", 1)[-1]
                    if symbol not in defined:
                        continue
                    path = self.call_path(surface, edges, symbol)
                    if path is not None:
                        self.fail(
                            f"{surface['id']}.{field} claims {symbol} is unreached, but "
                            f"{' -> '.join(path)} is a call path through the listed files"
                        )

    def test_rows_do_not_deny_a_call_path_a_listed_python_test_can_take(self) -> None:
        """A "no test reaches this reader" claim is walked, not trusted.

        Only a syntactic call path through the row's own source and test files is
        proved, to ``CALL_DEPTH`` hops; that is enough to refuse a claim of
        non-reachability, and it never claims the path is taken at run time.
        """
        self.assert_no_unreached_claims(self.inventory)

    def test_deliberately_false_never_reaches_claim_is_rejected(self) -> None:
        """A direct parser call in a listed test defeats a false gap claim."""
        inventory = copy.deepcopy(self.inventory)
        archive_row = next(
            row for row in inventory["surfaces"]
            if row["id"] == "python-xb-archive-extractor"
        )
        archive_row["resource_gaps"].append(
            "False self-test: the listed parser test never reaches XBArchiveReader.from_bytes."
        )

        with self.assertRaisesRegex(AssertionError, "claims from_bytes is unreached"):
            self.assert_no_unreached_claims(inventory)

    def python_call_edges(self, rel: str) -> dict[str, set[str]]:
        """Map every function defined in a Python file to the names it calls."""
        owner = self.enclosing_function(rel)
        edges: dict[str, set[str]] = {}
        for number, line in enumerate(self.source_lines(rel), 1):
            name = owner.get(number)
            if name is None:
                continue
            edges.setdefault(name, set()).update(self.PY_CALL.findall(line))
        return edges

    def call_path(
        self, surface: dict, edges: dict[str, dict[str, set[str]]], target: str
    ) -> list[str] | None:
        """Return one call chain from a listed test file to *target*, or None.

        The chain is a syntactic over-approximation: a name that a function body
        spells as a call counts even if that branch never executes, which is the
        safe direction for refusing a non-reachability claim.
        """
        frontier: dict[str, list[str]] = {}
        for rel in surface["test_paths"]:
            if rel.endswith(".py"):
                for name in self.PY_CALL.findall("\n".join(self.source_lines(rel))):
                    frontier.setdefault(name, [rel, name])
        if target in frontier:
            return frontier[target]
        seen = set(frontier)
        for _ in range(self.CALL_DEPTH):
            reached: dict[str, list[str]] = {}
            for name, path in frontier.items():
                for rel in edges:
                    for called in edges[rel].get(name, set()):
                        reached.setdefault(called, path + [called])
            if target in reached:
                return reached[target]
            frontier = {name: path for name, path in reached.items() if name not in seen}
            seen.update(frontier)
            if not frontier:
                return None
        return None

    def test_every_non_header_source_path_is_named_in_its_row(self) -> None:
        """A listed implementation file has to have a stated role and boundary."""
        for surface in self.inventory["surfaces"]:
            narrative = json.dumps(
                {
                    key: value
                    for key, value in surface.items()
                    if key not in {"source_paths", "test_paths"}
                }
            )
            for ref in surface["source_paths"]:
                if ref.endswith((".h", ".hpp")):
                    continue
                with self.subTest(surface=surface["id"], source=ref):
                    self.assertIn(
                        ref,
                        narrative,
                        f"{surface['id']} lists {ref} but never says what it parses "
                        "or which resource bound applies to it",
                    )

    def test_font_cache_row_states_the_real_pgf_boundary(self) -> None:
        """nk_font_check_cache caps the manifest; nk_font_validate_pgf does not.

        The digest helper streams the whole PGF through a fixed 64 KiB buffer, so
        its peak memory is bounded while the accepted input size is not. A row
        that calls the PGF "bounded by nk_font_validate_pgf" claims a cap the
        source does not apply.
        """
        font_c = "\n".join(self.source_lines("src/core/nk_font.c"))
        self.assertIn("mlen > 1024 * 1024", font_c)
        self.assertIn("fread(buffer, 1, sizeof(buffer), f)", font_c)
        max_input = self.row("nk-font-cache-manifest")["max_input"]
        self.assertNotIn("bounded by", max_input)
        self.assertIn("not byte-capped", max_input)
        self.assertIn("src/core/nk_font.c:nk_font_check_cache", max_input)

    def test_player_settings_row_matches_the_native_selftest_cases(self) -> None:
        """The settings selftest writes a corrupt document and a wrong schema.

        No megabyte-scale constant appears anywhere in the native selftest and it
        never writes an empty settings file, so the row cannot list an oversized
        or empty document as covered.
        """
        test_c = "\n".join(self.source_lines("tests/native/test_player_state.c"))
        self.assertIn("{ invalid_json: [1, 2, ", test_c)
        self.assertIn('\\"schema_version\\": 999', test_c)
        for absent in ("1024 * 1024", "1 << 20", "(1<<20)"):
            self.assertNotIn(absent, test_c, f"the selftest now builds {absent}; revisit the row")
        self.assertNotIn('write_text_file(test_settings_path, "")', test_c)
        row = self.row("player-settings-json")
        covered = " ".join(row["malformed_input_coverage"]).lower()
        for untested in ("oversize", "truncated", "empty"):
            self.assertNotIn(untested, covered)
        self.assertIn("oversized", " ".join(row["resource_gaps"]).lower())

    def test_inventory_does_not_claim_unrun_campaign_evidence(self) -> None:
        """The inventory must not imply sanitizer, coverage-guided, or kill coverage."""
        for surface in self.inventory["surfaces"]:
            campaigns = surface["campaigns"]
            with self.subTest(surface=surface["id"]):
                self.assertEqual(campaigns["coverage_guided_fuzzer"], "NOT_CONFIGURED")
                self.assertEqual(campaigns["overflow_oob_mutation_kill"], "NOT_RUN")
                seeded = campaigns["seeded_mutations"]
                if seeded["status"] == "NOT_AVAILABLE":
                    self.assertEqual(seeded["tests"], [])
                else:
                    # An in-CI seeded campaign has to say where the iteration count
                    # comes from. A row that only repeats the default sentence is
                    # claiming a campaign the runner never asks for.
                    self.assertTrue(seeded["tests"], surface["id"])
                    self.assertRegex(
                        seeded["campaign"].lower(),
                        r"iteration|mutation loop|mutations",
                        f"{surface['id']} claims an in-CI seeded campaign without saying how",
                    )
        self.assertIn(
            "NOT_RUN", self.inventory["campaign_facts"]["overflow_oob_mutation_kill"]
        )


if __name__ == "__main__":
    unittest.main()
