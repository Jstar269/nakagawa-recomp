# SPDX-License-Identifier: GPL-2.0-or-later
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

import random
import shutil
import struct
import tempfile
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import analyze  # noqa: E402
import imports  # noqa: E402
import prxload  # noqa: E402
from test_import_name_safety import build_synthetic_import_prx  # noqa: E402


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


def exercise(data: bytes, base: int, path: Path) -> dict[str, object]:
    """Run the parser pipeline and report the stage reached by the input.

    A rejected stage stops the dependent stages and marks them ``not_reached``;
    it is not reported as if every parser had consumed the same bytes.
    """
    path.write_bytes(data)
    report = {
        "prx": "not_reached",
        "analyze": "not_reached",
        "imports": "not_reached",
        "module_info": False,
        "import_count": None,
    }

    def load_prx():
        prx = prxload.Prx(str(path), base)
        prx.relocate()
        return prx

    status, _ = _run_stage("prx", load_prx, data)
    report["prx"] = status
    if status == "rejected":
        return report

    status, elf = _run_stage(
        "analyze", lambda: analyze.Elf(str(path), base), data
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


if __name__ == "__main__":
    unittest.main()
