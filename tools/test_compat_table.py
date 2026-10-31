# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""tools/compat_table.py: the tier rules, and that no identifier of a private disc leaves it."""

from __future__ import annotations

import json
from pathlib import Path
import re
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import compat_table  # noqa: E402
from nk_core import disc_verify  # noqa: E402

ROOT = TOOLS.parent
COMMIT = "0123456789abcdef0123456789abcdef01234567"


def record(disc_id: str, title: str, outcome: str, *, frames: int = 0, failure: str | None = None,
           tier: str | None = None, sha1: str = "ab" * 20) -> dict:
    """One manifest record shaped as tools/library_build.py writes it, with identifying text."""
    source = f"{title} [fan patch]/{title} (Japan).iso"
    built = outcome == "BUILT"
    row = {
        "source_key": source.casefold(), "source": source, "size_bytes": 1, "mtime_ns": 1,
        "outcome": outcome, "disc_id": disc_id, "title": title, "disc_version": "1.00",
        "identify_error": None, "work_dir": f"G:\\Library\\work\\{disc_id}-0123456789",
        "failure_class": failure if not built else "NONE",
        "run_settings": {"title_timeout_seconds": 2400, "launch_timeout_seconds": 30,
                         "source_commit": COMMIT},
    }
    if built:
        row["launch"] = {"presentation": "FRAME_SUBMITTED" if frames else "NO_FRAME_SUBMISSIONS",
                         "frame_submissions": frames, "failure_class": failure}
    if tier is not None:
        row["verification"] = {"sha1": sha1, "crc32": "cafe0123", "size_bytes": 1, "tier": tier,
                               "redump_name": f"{title} (Japan)"}
    return row


def manifest(titles: list[dict]) -> dict:
    return {"format": "nakagawa-library-build", "schema_version": 1,
            "updated_at": "2026-10-31T00:00:00Z", "disc_root": "G:\\Private Games",
            "user_data_root": "G:\\Private Data",
            "datafile": {"name": "Sony - PlayStation Portable", "version": "2026-06-13",
                         "discs": 3500, "checked_at": "2026-10-31T00:00:00Z"},
            "titles": titles}


SAMPLE = manifest([
    record("TEST71001", "Silver Lantern", "BUILT", frames=12, failure="LAUNCH_TIMEOUT",
           tier=disc_verify.VERIFIED_RETAIL),
    record("TEST71001", "Silver Lantern", "BUILT", frames=0, failure="UNSUPPORTED_IMPORT",
           tier=disc_verify.MODIFIED, sha1="cd" * 20),
    record("TEST71002", "Quiet Harbor", "BUILD_FAILED", failure="COMPILE_FAILED",
           tier=disc_verify.UNKNOWN),
    record("TEST71003", "Lantern Road", "TIMED_OUT", failure="TITLE_TIME_BUDGET"),
    record("TEST71004", "Glass Orchard", "BUILT", frames=3, failure="A_PRIVATE_FREE_TEXT_CAUSE",
           tier=disc_verify.VERIFIED_RETAIL),
    record("TEST71005", "Copper Tide", "PENDING"),
    record("TEST71006", "Copper Tide Again", "DUPLICATE"),
])


class TierRuleTests(unittest.TestCase):
    def test_each_outcome_lands_in_its_tier(self) -> None:
        self.assertEqual(compat_table.tier_of(record("A", "B", "BUILD_FAILED")),
                         compat_table.NOT_BUILT)
        self.assertEqual(compat_table.tier_of(record("A", "B", "TIMED_OUT")), compat_table.NOT_BUILT)
        self.assertEqual(compat_table.tier_of(record("A", "B", "NO_REPORT")), compat_table.NOT_BUILT)
        self.assertEqual(compat_table.tier_of(record("A", "B", "BUILT")), compat_table.NO_FRAME)
        self.assertEqual(compat_table.tier_of(record("A", "B", "BUILT", frames=1)),
                         compat_table.DRAWS_FRAMES)
        self.assertIsNone(compat_table.tier_of(record("A", "B", "PENDING")))
        self.assertIsNone(compat_table.tier_of(record("A", "B", "RUNNING")))
        self.assertIsNone(compat_table.tier_of(record("A", "B", "DUPLICATE")))

    def test_counts_by_tier_verification_and_cause(self) -> None:
        summary = compat_table.summarize(SAMPLE)
        self.assertEqual(summary["counted"], 5)
        tiers = summary["tiers"]
        self.assertEqual(tiers[compat_table.DRAWS_FRAMES][disc_verify.VERIFIED_RETAIL], 2)
        self.assertEqual(tiers[compat_table.NO_FRAME][disc_verify.MODIFIED], 1)
        self.assertEqual(tiers[compat_table.NOT_BUILT][disc_verify.UNKNOWN], 1)
        self.assertEqual(tiers[compat_table.NOT_BUILT][disc_verify.NOT_CHECKED], 1)
        self.assertEqual(summary["left_out"], {"not_built_yet": 1, "copies": 1})
        causes = {(row["tier"], row["cause"]): (row["discs"], row["issue"])
                  for row in summary["causes"]}
        self.assertEqual(causes[(compat_table.NOT_BUILT, "COMPILE_FAILED")], (1, 1730))
        self.assertEqual(causes[(compat_table.NO_FRAME, "UNSUPPORTED_IMPORT")], (1, 308))
        # A cause outside the failure-code taxonomy is never printed by name.
        self.assertEqual(causes[(compat_table.DRAWS_FRAMES, compat_table.OTHER_CAUSE)], (1, 308))
        self.assertEqual(summary["settings"], [{"title_timeout_seconds": 2400,
                                                "launch_timeout_seconds": 30,
                                                "source_commit": COMMIT[:12], "discs": 5}])

    def test_higher_tiers_say_not_measured_and_settings_are_named(self) -> None:
        block = compat_table.render_markdown(compat_table.summarize(SAMPLE), "2026-10-31")
        self.assertIn("| Reaches menus | not measured |", block)
        self.assertIn("| Playable start to end | not measured |", block)
        self.assertIn("commit `0123456789ab`, with a 2400 s time budget per disc and a 30 s "
                      "first launch", block)
        self.assertIn("3,500 discs, version 2026-06-13", block)
        self.assertIn("1 not built yet in this run, and 1 copy of a disc already counted.", block)
        # The list in SAMPLE records no serials, so the summary says Unknown is not Modified.
        self.assertIn("A lone fan translation counts as **Unknown**", block)
        with_serials = json.loads(json.dumps(SAMPLE))
        with_serials["datafile"]["serials"] = 12
        self.assertNotIn("lone fan translation",
                         compat_table.render_markdown(compat_table.summarize(with_serials),
                                                      "2026-10-31"))
        mixed = json.loads(json.dumps(SAMPLE))
        mixed["titles"][0]["run_settings"]["title_timeout_seconds"] = 5400
        self.assertIn("not all built with the same settings",
                      compat_table.render_markdown(compat_table.summarize(mixed), "2026-10-31"))


class PrivacyTests(unittest.TestCase):
    def test_no_identifier_of_the_input_reaches_the_summary(self) -> None:
        block = compat_table.render_markdown(compat_table.summarize(SAMPLE), "2026-10-31")
        payload = json.dumps(compat_table.summarize(SAMPLE))
        compat_table.check_public(block, SAMPLE)
        compat_table.check_public(payload, SAMPLE)
        for text in (block, payload):
            for row in SAMPLE["titles"]:
                for value in (row["disc_id"], row["title"], row["source"], row["work_dir"]):
                    self.assertNotIn(value.casefold(), text.casefold())
                verification = row.get("verification") or {}
                for value in (verification.get("sha1"), verification.get("redump_name")):
                    if value:
                        self.assertNotIn(value.casefold(), text.casefold())
            self.assertNotIn("A_PRIVATE_FREE_TEXT_CAUSE", text)
            self.assertIsNone(re.search(r"[A-Z]{4}-?[0-9]{5}", text))
            self.assertNotIn("Private", text)

    def test_the_guard_refuses_a_summary_that_names_a_disc(self) -> None:
        block = compat_table.render_markdown(compat_table.summarize(SAMPLE), "2026-10-31")
        for leak in ("Quiet Harbor", "quiet harbor", "TEST71001", "TEST-71001", "ab" * 20,
                     "G:\\Private Games", "Lantern Road (Japan)"):
            with self.subTest(leak=leak), self.assertRaises(compat_table.PrivacyError):
                compat_table.check_public(block + f"\n{leak}\n", SAMPLE)
        # Folders named only with the summary's own words don't stop it.
        plain = manifest([record("TEST71008", "Quiet Field", "BUILT", frames=1)])
        plain["titles"][0]["source"] = "Library/Modified/Redump/Quiet Field (Japan).iso"
        compat_table.check_public(
            compat_table.render_markdown(compat_table.summarize(plain), "2026-10-31"), plain)
        # Free text in the datafile's version field is never published.
        noted = json.loads(json.dumps(SAMPLE))
        noted["datafile"]["version"] = "my private note"
        self.assertIsNone(compat_table.summarize(noted)["datafile"]["version"])
        self.assertNotIn("private note", compat_table.render_markdown(
            compat_table.summarize(noted), "2026-10-31"))
        # Settings of another shape count as unrecorded, never as text or a crash.
        odd = json.loads(json.dumps(SAMPLE))
        odd["titles"][0]["run_settings"] = {"title_timeout_seconds": "2400 private",
                                            "launch_timeout_seconds": [30],
                                            "source_commit": "x"}
        rows = compat_table.summarize(odd)["settings"]
        self.assertIn({"title_timeout_seconds": None, "launch_timeout_seconds": None,
                       "source_commit": None, "discs": 1}, rows)
        # A disc named like part of a cause name is not a leak by itself.
        named_exit = manifest([record("TEST71007", "EXIT", "BUILT", failure="EXITED_ZERO_BEFORE_HLE")])
        compat_table.check_public(
            compat_table.render_markdown(compat_table.summarize(named_exit), "2026-10-31"),
            named_exit)

    def test_cli_writes_the_document_block_and_json(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "library-build.json"
            source.write_text(json.dumps(SAMPLE), encoding="utf-8")
            doc = root / "COMPATIBILITY.md"
            doc.write_text(f"# Compatibility\n\nBefore.\n\n{compat_table.DOC_BEGIN}\nold\n"
                           f"{compat_table.DOC_END}\n\nAfter.\n", encoding="utf-8")
            self.assertEqual(compat_table.main([str(source), "--write-doc", str(doc),
                                                "--json", str(root / "summary.json")]), 0)
            text = doc.read_text(encoding="utf-8")
            self.assertTrue(text.startswith("# Compatibility\n\nBefore.\n\n"))
            self.assertTrue(text.endswith(f"{compat_table.DOC_END}\n\nAfter.\n"))
            self.assertNotIn("\nold\n", text)
            self.assertIn("Whole library, 2026-10-31: 5 disc images counted.", text)
            self.assertEqual(json.loads((root / "summary.json").read_text(encoding="utf-8"))
                             ["counted"], 5)

    def test_the_compatibility_page_has_one_generated_block(self) -> None:
        text = (ROOT / "docs" / "COMPATIBILITY.md").read_text(encoding="utf-8")
        self.assertEqual(text.count(compat_table.DOC_BEGIN), 1)
        self.assertEqual(text.count(compat_table.DOC_END), 1)
        start = text.index(compat_table.DOC_BEGIN)
        block = text[start:text.index(compat_table.DOC_END) + len(compat_table.DOC_END)]
        for shape in compat_table._IDENTIFIER_SHAPES:
            self.assertIsNone(shape.search(block))


if __name__ == "__main__":
    unittest.main()
