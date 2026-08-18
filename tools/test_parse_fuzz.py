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


def valid_seed_elf() -> bytes:
    """A minimal but structurally complete ELF: header, PT_LOAD, and one
    type-A relocation section so mutations can reach the relocation code."""
    phoff = 52
    blob = bytearray(phoff + 32 * 2 + 64)  # 2 phdrs + payload
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into("<H", blob, 16, 2)  # ET_EXEC
    struct.pack_into("<III", blob, 24, 0, phoff, 0)
    struct.pack_into("<HHHHH", blob, 42, 32, 2, 0, 0, 0)
    # PT_LOAD at vaddr 0x08000000 with a 64-byte payload.
    struct.pack_into("<8I", blob, phoff, 1, phoff + 64, 0x08000000, 0, 64, 64, 5, 4)
    # A relocation-type program header (the analyzer's section fallback).
    struct.pack_into("<8I", blob, phoff + 32, 0x700000A0, phoff + 64, 0, 0, 64, 64, 3, 4)
    # All-zero reloc payload: (offset=0, info=0) pairs are benign (R_MIPS_NONE),
    # so the seed passes prxload.relocate() and mutations can reach deep paths.
    blob[phoff + 64:] = b"\x00" * 64
    return bytes(blob)


def exercise(data: bytes, base: int, path: Path) -> str:
    """Run every parser over one input; ValueError is the only acceptable
    failure mode. Returns 'rejected' or 'accepted'."""
    path.write_bytes(data)
    try:
        prx = prxload.Prx(str(path), base)
        prx.relocate()
    except ValueError:
        return "rejected"
    try:
        elf = analyze.Elf(str(path))
        imports.parse_imports(elf)
    except ValueError:
        pass
    return "accepted"


def describe(data: bytes) -> str:
    head = data[:32].hex()
    import hashlib

    return f"len={len(data)} sha256={hashlib.sha256(data).hexdigest()[:16]} head={head}"


class TestParseFuzz(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="parsefuzz_")
        cls.path = Path(cls.tmp) / "fuzz.elf"

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_random_blob_stream_never_escapes_valueerror(self) -> None:
        rng = random.Random(0x5EE7)
        for _ in range(1500):
            n = rng.randint(1, 2048)
            data = rng.randbytes(n)
            base = rng.choice([0, 0x08000000, 0x08004000, 0xFFFFFFFF])
            try:
                exercise(data, base, self.path)
            except Exception as exc:  # noqa: BLE001 - any escape is the bug
                self.fail(f"parser escaped ValueError as {exc!r} for {describe(data)}")

    def test_seed_mutation_stream_never_escapes_valueerror(self) -> None:
        rng = random.Random(0x51F7)
        seed = bytearray(valid_seed_elf())
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
            base = rng.choice([0, 0x08000000])
            try:
                exercise(bytes(data), base, self.path)
            except Exception as exc:  # noqa: BLE001 - any escape is the bug
                self.fail(f"parser escaped ValueError as {exc!r} for {describe(bytes(data))}")

    def test_valid_seed_is_accepted(self) -> None:
        self.assertEqual(exercise(valid_seed_elf(), 0x08000000, self.path), "accepted")


if __name__ == "__main__":
    unittest.main()
