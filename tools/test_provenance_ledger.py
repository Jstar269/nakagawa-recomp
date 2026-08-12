#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Structural gate for docs/provenance/IMPLEMENTATION_PROVENANCE.json.

The ledger is a human judgment; this gate cannot check whether a classification
is honest.  What it can check is that the ledger stays complete and internally
consistent as the tree moves:

- every tracked file under ``src/`` is covered by exactly one record;
- every classification comes from the closed vocabulary;
- no file whose header declares ``Derived from <project>`` sits in a record
  classified ``project-authored-independent``;
- every record naming an upstream project also records a license;
- every finding referenced from a record's ``uncertainty`` text exists.

Run directly or through ``python -m unittest discover -s tools``.
"""

from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from build_public_export import public_safe_excluded_paths  # noqa: E402
import provenance_ledger  # noqa: E402
LEDGER = ROOT / "docs" / "provenance" / "IMPLEMENTATION_PROVENANCE.json"
PUBLIC_LEDGER = ROOT / "assets" / "public_provenance_ledger.json"

VOCABULARY = {
    "project-authored-independent",
    "behavior-informed",
    "derived-translated",
    "derived-data",
    "generated-project-owned",
    "upstream-third-party",
    "unresolved",
}

REPLACEMENT_STATES = {
    "not-started",
    "specified",
    "in-progress",
    "replaced",
    "blocked",
    "not-applicable",
}

DERIVED_HEADER = re.compile(r"Derived from\s+(\S+)", re.IGNORECASE)
FINDING_REF = re.compile(r"\b(PROV-F\d+|IND-\d+)\b")

# Extensions whose leading comment block is a provenance header we can read.
SOURCE_SUFFIXES = {".c", ".h", ".cpp", ".hpp", ".py"}


def tracked_files(prefix: str) -> list[str]:
    out = subprocess.run(
        ["git", "ls-files", prefix],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return [line for line in out.stdout.splitlines() if line.strip()]


def load_ledger() -> dict:
    if LEDGER.is_file():
        return json.loads(LEDGER.read_text(encoding="utf-8"))
    return json.loads(PUBLIC_LEDGER.read_text(encoding="utf-8"))


class ProvenanceLedgerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.ledger = load_ledger()
        cls.public_only = not LEDGER.is_file()
        cls.records = cls.ledger.get("records", [])
        cls.public_entries = cls.ledger.get("entries", [])

    def test_vocabulary_matches_model(self) -> None:
        if self.public_only:
            self.assertEqual(
                set(self.ledger["classification_vocabulary"]),
                {
                    "project_authored_attested",
                    "upstream_derived",
                    "generated_from_public_source",
                    "synthetic_fixture",
                    "public_factual_metadata",
                    "reviewed_configuration",
                    "reviewed_documentation",
                    "reviewed_other",
                    "unresolved",
                },
            )
            return
        self.assertEqual(set(self.ledger["vocabulary"]), VOCABULARY)

    def test_record_ids_unique(self) -> None:
        if self.public_only:
            paths = [entry.get("path") for entry in self.public_entries]
            self.assertEqual(len(paths), len(set(paths)), "duplicate public ledger path")
            return
        ids = [r["id"] for r in self.records]
        self.assertEqual(len(ids), len(set(ids)), "duplicate record id")

    def test_classifications_are_in_vocabulary(self) -> None:
        if self.public_only:
            allowed = set(self.ledger["classification_vocabulary"])
            for entry in self.public_entries:
                self.assertIn(entry.get("classification"), allowed, entry.get("path"))
            return
        for rec in self.records:
            self.assertIn(
                rec["classification"],
                VOCABULARY,
                f"{rec['id']}: classification outside the closed vocabulary",
            )

    def test_replacement_states_are_known(self) -> None:
        if self.public_only:
            return
        for rec in self.records:
            self.assertIn(
                rec["replacement_state"],
                REPLACEMENT_STATES,
                f"{rec['id']}: unknown replacement_state",
            )

    def test_named_upstream_carries_a_license(self) -> None:
        if self.public_only:
            return
        for rec in self.records:
            if rec.get("upstream"):
                self.assertIn(
                    rec["upstream"],
                    self.ledger["upstreams"],
                    f"{rec['id']}: upstream not declared in the upstreams map",
                )
                self.assertTrue(
                    rec.get("upstream_license"),
                    f"{rec['id']}: names an upstream but records no license",
                )

    def test_every_src_file_is_covered_exactly_once(self) -> None:
        if self.public_only:
            policy = json.loads((ROOT / "assets" / "public_source_profile.json").read_text(encoding="utf-8"))
            included = set(policy["include_paths"])
            public_paths = {entry.get("path") for entry in self.public_entries}
            src_paths = set(tracked_files("src")) & included
            self.assertEqual(src_paths, public_paths & set(tracked_files("src")))
            return
        covered: dict[str, list[str]] = {}
        for rec in self.records:
            for path in rec["paths"]:
                if path.endswith("/*"):
                    continue
                covered.setdefault(path, []).append(rec["id"])

        missing = []
        for path in tracked_files("src"):
            owners = covered.get(path, [])
            if not owners:
                missing.append(path)
            else:
                self.assertEqual(
                    len(owners),
                    1,
                    f"{path}: claimed by multiple records {owners}",
                )
        self.assertEqual(
            missing,
            [],
            "tracked src/ files with no provenance record: "
            + ", ".join(missing),
        )

    def test_no_ledger_path_is_stale(self) -> None:
        if self.public_only:
            tracked = set(tracked_files("."))
            policy = json.loads((ROOT / "assets" / "public_source_profile.json").read_text(encoding="utf-8"))
            included = set(policy["include_paths"])
            for entry in self.public_entries:
                path = entry.get("path")
                self.assertIn(path, tracked, path)
                self.assertIn(path, included, path)
            return
        tracked = set(tracked_files("src")) | set(tracked_files("tools"))
        tracked |= set(tracked_files("assets")) | set(tracked_files("font"))
        tracked |= set(tracked_files("THIRD_PARTY_LICENSES"))
        tracked |= set(tracked_files("fixtures"))
        # In a materialized public-safe export the profile-excluded components
        # are absent by design, so the ledger legitimately outlives them. Every
        # other ledger path is still required to be tracked.
        # Ask the canonical policy whether a path is excluded. The generated
        # export cannot answer this for glob-matched paths, and the policy is the
        # authority in any case.
        import publication_policy

        _policy = publication_policy.load_policy(ROOT / "assets" / "public_source_profile.json")
        excluded = public_safe_excluded_paths(ROOT)
        stale = []
        for rec in self.records:
            for path in rec["paths"]:
                if path.endswith("/*"):
                    continue
                if path in excluded or _policy.resolve(path).is_excluded:
                    continue
                if path not in tracked:
                    stale.append(f"{rec['id']}:{path}")
        self.assertEqual(stale, [], "ledger references untracked paths: " + ", ".join(stale))

    def test_derived_headers_are_not_called_independent(self) -> None:
        """A file that says it is derived may not sit in an 'independent' record."""
        if self.public_only:
            return
        by_path = {}
        for rec in self.records:
            for path in rec["paths"]:
                by_path[path] = rec

        offenders = []
        for path in tracked_files("src"):
            if Path(path).suffix not in SOURCE_SUFFIXES:
                continue
            text = (ROOT / path).read_text(encoding="utf-8", errors="replace")
            head = "\n".join(text.splitlines()[:12])
            if not DERIVED_HEADER.search(head):
                continue
            rec = by_path.get(path)
            self.assertIsNotNone(rec, f"{path}: declares derivation but has no record")
            if rec["classification"] == "project-authored-independent":
                offenders.append(f"{path} -> {rec['id']}")
        self.assertEqual(
            offenders,
            [],
            "files declaring derivation inside a project-authored-independent record: "
            + ", ".join(offenders),
        )

    def test_finding_references_resolve(self) -> None:
        if self.public_only:
            for entry in self.public_entries:
                self.assertTrue(entry.get("evidence"), entry.get("path"))
            return
        known = {f["id"] for f in self.ledger["findings"]}
        backlog = ROOT / "docs" / "provenance" / "INDEPENDENCE_BACKLOG.md"
        backlog_text = backlog.read_text(encoding="utf-8") if backlog.exists() else ""
        known |= set(FINDING_REF.findall(backlog_text))

        unknown = set()
        for rec in self.records:
            for note in rec.get("uncertainty", []):
                for ref in FINDING_REF.findall(note):
                    if ref not in known:
                        unknown.add(f"{rec['id']}:{ref}")
        self.assertEqual(
            unknown,
            set(),
            "records cite identifiers that exist nowhere: " + ", ".join(sorted(unknown)),
        )

    def test_findings_are_well_formed(self) -> None:
        if self.public_only:
            self.assertTrue(self.public_entries, "public provenance ledger must not be empty")
            for entry in self.public_entries:
                if entry.get("path") not in {"PUBLIC_EXPORT.json", "assets/public_provenance_ledger.json"}:
                    self.assertEqual(len(entry.get("sha256", "")), 64, entry.get("path"))
                self.assertNotEqual(entry.get("classification"), "unresolved", entry.get("path"))
            return
        for finding in self.ledger["findings"]:
            for field in ("id", "severity", "title", "detail", "action"):
                self.assertTrue(
                    finding.get(field),
                    f"{finding.get('id', '<no id>')}: missing {field}",
                )
                self.assertIn(finding["severity"], {"low", "medium", "high"})


class ProvenanceLedgerGeneratorTest(unittest.TestCase):
    def test_external_detailed_ledger_is_read_without_recording_its_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            detailed = Path(temp_dir) / "private-evidence.json"
            detailed.write_text(
                json.dumps(
                    {
                        "records": [
                            {
                                "id": "PROV-TEST",
                                "classification": "project-authored-independent",
                                "paths": ["src/example.c"],
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )
            records = provenance_ledger._implementation_records(detailed)
        self.assertEqual(records["src/example.c"]["id"], "PROV-TEST")
        self.assertNotIn(str(detailed), json.dumps(records))


if __name__ == "__main__":
    unittest.main()
