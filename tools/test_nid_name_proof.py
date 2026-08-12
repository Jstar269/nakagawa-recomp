#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for the independent NID-name verifier.

The classifier decides what the project may honestly SAY about a table entry, so
its failure modes matter as much as its successes.  In particular it must not
over-match: calling a real export an "editorial alias" would understate the
table's factual content, and calling an unknown entry "verified" would overstate
its independence.
"""

from __future__ import annotations

import hashlib
import unittest

import nid_name_proof as proof


def make_header(pairs: list[tuple[int, str]]) -> str:
    body = "\n".join(f'    {{0x{nid:08x}u, "{name}"}},' for nid, name in pairs)
    return "static const SrNidName sr_nid_table[] = {\n" + body + "\n};\n"


class NidDerivationTest(unittest.TestCase):
    def test_rule_is_little_endian_sha1_prefix(self) -> None:
        name = "sceKernelSetCompiledSdkVersion"
        digest = hashlib.sha1(name.encode()).digest()
        self.assertEqual(proof.nid_of(name), int.from_bytes(digest[:4], "little"))

    def test_rule_is_not_big_endian(self) -> None:
        """Guards against a byte-order regression that would silently pass most
        entries only if the table were also regenerated wrongly."""
        name = "sceIoOpen"
        digest = hashlib.sha1(name.encode()).digest()
        self.assertNotEqual(proof.nid_of(name), int.from_bytes(digest[:4], "big"))

    def test_distinct_names_do_not_collide(self) -> None:
        names = ["sceIoOpen", "sceIoClose", "sceIoRead", "sceIoWrite"]
        self.assertEqual(len({proof.nid_of(n) for n in names}), len(names))


class ClassifierTest(unittest.TestCase):
    def test_verified_entry(self) -> None:
        name = "sceIoOpen"
        entries = proof.classify_all([(proof.nid_of(name), name)])
        self.assertEqual(entries[0].classification, "verified")

    def test_wrong_nid_for_real_name_is_unresolved_not_wrong(self) -> None:
        """A failed derivation must never be reported as a refutation."""
        entries = proof.classify_all([(0x12345678, "sceIoOpen")])
        self.assertEqual(entries[0].classification, "unresolved")
        self.assertIn("neither confirmed nor refuted", entries[0].rationale)

    def test_library_placeholder_is_recognised(self) -> None:
        entries = proof.classify_all([(0x043EBE3E, "sceUtility_043ebe3e")])
        self.assertEqual(entries[0].classification, "library-attributed-unknown-name")
        self.assertIn("sceUtility", entries[0].rationale)

    def test_placeholder_must_encode_its_own_nid(self) -> None:
        """A name that merely looks hex-suffixed is not a placeholder."""
        entries = proof.classify_all([(0x11111111, "sceUtility_043ebe3e")])
        self.assertNotEqual(
            entries[0].classification, "library-attributed-unknown-name"
        )

    def test_sentinel_nid_is_recognised(self) -> None:
        entries = proof.classify_all([(0xC0DE0001, "__UtilityFinishDialog")])
        self.assertEqual(entries[0].classification, "emulator-internal-sentinel")

    def test_alias_requires_a_verified_base_in_the_same_table(self) -> None:
        base = "scePowerSetClockFrequency"
        alias = base + "350"
        with_base = proof.classify_all(
            [(proof.nid_of(base), base), (0xEBD177D6, alias)]
        )
        self.assertEqual(with_base[1].classification, "editorial-alias")

        # Same alias, but the base is absent: must not be claimed as an alias.
        without_base = proof.classify_all([(0xEBD177D6, alias)])
        self.assertEqual(without_base[0].classification, "unresolved")

    def test_alias_base_must_itself_be_verified(self) -> None:
        """An unverified base cannot license an alias claim about its suffix."""
        base = "scePowerSetClockFrequency"
        entries = proof.classify_all(
            [(0xDEADC0D1, base), (0xEBD177D6, base + "350")]
        )
        self.assertEqual(entries[1].classification, "unresolved")


class ShippedTableTest(unittest.TestCase):
    """Locks the measurement the provenance ledger cites."""

    @classmethod
    def setUpClass(cls) -> None:
        text = proof.DEFAULT_HEADER.read_text(encoding="utf-8")
        cls.entries = proof.classify_all(proof.parse_header(text))
        cls.report = proof.build_report(cls.entries)

    def test_every_entry_is_classified(self) -> None:
        self.assertEqual(
            sum(self.report["counts"].values()), self.report["total"]
        )
        for entry in self.entries:
            self.assertIn(entry.classification, proof.CLASSIFICATIONS)

    def test_measurement_matches_the_ledger(self) -> None:
        counts = self.report["counts"]
        self.assertEqual(self.report["total"], 1615)
        self.assertEqual(counts["verified"], 1463)
        self.assertEqual(counts["library-attributed-unknown-name"], 54)
        self.assertEqual(counts["editorial-alias"], 10)
        self.assertEqual(counts["unresolved"], 88)

    def test_no_sentinels_remain_in_the_shipped_table(self) -> None:
        """IND-1 step A removed the eight; the generator cannot reintroduce them."""
        self.assertEqual(self.report["counts"]["emulator-internal-sentinel"], 0)

    def test_real_double_underscore_exports_survive(self) -> None:
        """The PSP's sceSasCore library genuinely exports __sceSas* names.

        A bare "__ prefix means emulator-internal" rule would delete 33 real
        exports.  A hash-verified NID is proof of non-sentinel allocation, so
        these must classify as verified and never as sentinels.
        """
        double = [e for e in self.entries if e.name.startswith("__")]
        self.assertEqual(len(double), 33)
        for entry in double:
            self.assertEqual(
                entry.classification,
                "verified",
                f"{entry.name} must stay verified, not be culled as a sentinel",
            )

    def test_no_verified_entry_appears_in_the_findings_list(self) -> None:
        names = {e["name"] for e in self.report["entries"]}
        verified = {e.name for e in self.entries if e.classification == "verified"}
        self.assertEqual(names & verified, set())


if __name__ == "__main__":
    unittest.main()
