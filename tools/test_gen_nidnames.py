#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Tests for the corpus-driven NID name-table generator.

The generator's job is narrow: recompute the NID from the name wherever that is
possible, store it only where it is not, and refuse to emit emulator-internal
sentinels.  The failure modes that matter are silently *storing* a derivable
NID (which would preserve a copied constant) and over-eagerly culling entries
that merely look internal (which would delete real PSP exports).
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import unittest

import gen_nidnames as gen

ROOT = Path(__file__).resolve().parents[1]


def corpus(entries: list[dict]) -> dict:
    counts = {c: 0 for c in ("pspsdk-sourced", "hash-verified",
                             "library-attributed", "editorial-alias", "unresolved")}
    for entry in entries:
        counts[entry["class"]] += 1
    return {"schema_version": 1, "counts": counts, "entries": entries}


class ResolveTest(unittest.TestCase):
    def test_derivable_entry_computes_its_nid(self) -> None:
        name = "sceIoOpen"
        nid, got = gen.resolve({"class": "hash-verified", "name": name})
        self.assertEqual(got, name)
        self.assertEqual(nid, int.from_bytes(hashlib.sha1(name.encode()).digest()[:4], "little"))

    def test_derivable_entry_may_not_store_a_nid(self) -> None:
        """Storing it would defeat the whole point: the constant must be absent."""
        with self.assertRaises(gen.CorpusError) as ctx:
            gen.resolve({"class": "hash-verified", "name": "sceIoOpen", "nid": "0x109f50bc"})
        self.assertIn("recomputed from the name", str(ctx.exception))

    def test_stored_entry_must_store_a_nid(self) -> None:
        with self.assertRaises(gen.CorpusError):
            gen.resolve({"class": "unresolved", "name": "sceNpInit"})

    def test_stored_entry_that_is_actually_derivable_is_rejected(self) -> None:
        """Guards against a copied constant hiding in a stored-class entry."""
        name = "sceIoOpen"
        nid = gen.nid_of(name)
        with self.assertRaises(gen.CorpusError) as ctx:
            gen.resolve({"class": "unresolved", "name": name, "nid": f"0x{nid:08x}"})
        self.assertIn("belongs in a derivable class", str(ctx.exception))

    def test_unknown_class_is_rejected(self) -> None:
        with self.assertRaises(gen.CorpusError):
            gen.resolve({"class": "vibes", "name": "sceIoOpen"})


class SentinelTest(unittest.TestCase):
    def test_non_derivable_sentinel_is_rejected(self) -> None:
        with self.assertRaises(gen.CorpusError):
            gen.check_not_sentinel(0xC0DE0001, "__UtilityFinishDialog", derivable=False)

    def test_non_derivable_reserved_identifier_is_rejected(self) -> None:
        with self.assertRaises(gen.CorpusError):
            gen.check_not_sentinel(0x13370001, "__IoAsyncFinish", derivable=False)

    def test_derivable_double_underscore_export_is_kept(self) -> None:
        """__sceSas* are real PSP exports. A hash-verified NID proves the entry
        was not sentinel-allocated, so the heuristic must not fire on it."""
        name = "__sceSasCore"
        gen.check_not_sentinel(gen.nid_of(name), name, derivable=True)

    def test_generator_refuses_a_sentinel_in_the_corpus(self) -> None:
        with self.assertRaises(gen.CorpusError):
            gen.render(corpus([
                {"class": "unresolved", "name": "__UtilityWorkUs", "nid": "0xc0de0002"},
            ]))


class RenderTest(unittest.TestCase):
    def test_duplicate_nids_are_rejected(self) -> None:
        name = "sceIoOpen"
        nid = gen.nid_of(name)
        with self.assertRaises(gen.CorpusError) as ctx:
            gen.render(corpus([
                {"class": "hash-verified", "name": name},
                {"class": "unresolved", "name": "sceSomethingElse", "nid": f"0x{nid:08x}"},
            ]))
        self.assertIn("duplicate NID", str(ctx.exception))

    def test_output_is_sorted_by_nid(self) -> None:
        text = gen.render(corpus([
            {"class": "hash-verified", "name": n}
            for n in ("sceIoOpen", "sceIoClose", "sceIoRead", "sceIoWrite")
        ]))
        nids = [int(line.split("{0x")[1][:8], 16) for line in text.splitlines()
                if line.strip().startswith("{0x")]
        self.assertEqual(nids, sorted(nids))

    def test_render_is_deterministic(self) -> None:
        data = corpus([{"class": "hash-verified", "name": "sceIoOpen"}])
        self.assertEqual(gen.render(data), gen.render(data))


class TrackedCorpusTest(unittest.TestCase):
    """The shipped corpus and the shipped header must agree exactly."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.corpus = json.loads(gen.CORPUS.read_text(encoding="utf-8"))

    def test_header_is_up_to_date(self) -> None:
        self.assertEqual(
            gen.render(self.corpus),
            gen.HEADER.read_text(encoding="utf-8"),
            "src/rt/nid_names.h is stale; rerun tools/gen_nidnames.py",
        )

    def test_derivable_entries_store_no_nid(self) -> None:
        stored = [
            e["name"] for e in self.corpus["entries"]
            if e["class"] in gen.DERIVABLE_CLASSES and "nid" in e
        ]
        self.assertEqual(stored, [], "derivable entries must not carry a copied NID")

    def test_counts_match_entries(self) -> None:
        for cls, expected in self.corpus["counts"].items():
            actual = sum(1 for e in self.corpus["entries"] if e["class"] == cls)
            self.assertEqual(actual, expected, f"count mismatch for {cls}")

    def test_copied_constant_reduction_is_what_it_claims(self) -> None:
        derived = sum(self.corpus["counts"][c] for c in gen.DERIVABLE_CLASSES)
        stored = sum(1 for e in self.corpus["entries"] if "nid" in e)
        self.assertEqual(derived, 1463)
        self.assertEqual(stored, 152)
        self.assertEqual(derived + stored, len(self.corpus["entries"]))

    def test_generator_needs_no_ppsspp_checkout(self) -> None:
        """Phase E: the tracked corpus is the only input."""
        source = gen.__file__ and Path(gen.__file__).read_text(encoding="utf-8")
        self.assertNotIn("third_party/ppsspp", source)
        self.assertNotIn("Core/HLE/*.cpp", source)


if __name__ == "__main__":
    unittest.main()
