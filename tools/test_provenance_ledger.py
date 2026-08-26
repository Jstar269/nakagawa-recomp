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

Classification is also fail-closed: an implementation-bearing path with no
path-specific record resolves to ``unresolved``, wildcard records such as
``tools/*`` are never expanded, and the generator refuses to write release
evidence while any included path is unresolved.

Run directly or through ``python -m unittest discover -s tools``.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

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


class ProvenanceFailClosedTest(unittest.TestCase):
    """Adversarial suite for fail-closed provenance classification.

    Every fixture is synthetic.  These tests pin the P0 property: an
    implementation-bearing path may never receive a
    ``project_authored_attested`` classification merely because no specific
    record exists, neither the historical ``tools/*`` wildcard nor a policy
    edit may self-authorize one, and release evidence may not be generated
    while any included path is unresolved.
    """

    # -- classification ------------------------------------------------------

    def test_new_unrecorded_src_c_file_is_unresolved(self) -> None:
        cls, evidence = provenance_ledger._class_for("src/rt/new_widget.c", None)
        self.assertEqual(cls, "unresolved")
        self.assertEqual(evidence["source"], "missing path-specific provenance record")
        self.assertNotEqual(cls, "project_authored_attested")

    def test_new_unrecorded_src_header_is_unresolved(self) -> None:
        cls, _ = provenance_ledger._class_for("src/rt/new_widget.h", None)
        self.assertEqual(cls, "unresolved")

    def test_new_unrecorded_implementation_tool_is_unresolved(self) -> None:
        cls, _ = provenance_ledger._class_for("tools/brand_new_tool.py", None)
        self.assertEqual(cls, "unresolved")

    def test_unrecorded_root_script_is_unresolved(self) -> None:
        cls, _ = provenance_ledger._class_for("hst.ps1", None)
        self.assertEqual(cls, "unresolved")

    def test_interface_implementation_is_not_wholesale_configuration(self) -> None:
        """The dashboard's src/ is implementation, not reviewed configuration."""
        self.assertEqual(
            provenance_ledger._class_for("interface/src/lib/new_module.ts", None)[0], "unresolved"
        )
        self.assertEqual(
            provenance_ledger._class_for("interface/src/app/api/route.ts", None)[0], "unresolved"
        )
        self.assertEqual(
            provenance_ledger._class_for("interface/package.json", None)[0], "reviewed_configuration"
        )
        self.assertEqual(
            provenance_ledger._class_for("interface/next.config.ts", None)[0], "reviewed_configuration"
        )

    def test_docs_config_fixture_metadata_rules_are_preserved(self) -> None:
        cases = {
            "docs/ARCHITECTURE.md": "reviewed_documentation",
            "tools/README.md": "reviewed_documentation",
            "src/rt/atrac3p/PROVENANCE.md": "reviewed_documentation",
            "README.md": "reviewed_documentation",
            "LICENSE": "reviewed_documentation",
            ".clang-format": "reviewed_configuration",
            ".gitignore": "reviewed_configuration",
            "Makefile": "reviewed_configuration",
            "mk/build_common.mk": "reviewed_configuration",
            ".github/workflows/ci.yml": "reviewed_configuration",
            "pyproject.toml": "reviewed_configuration",
            "interface/package-lock.json": "reviewed_configuration",
            "interface/prisma/schema.prisma": "reviewed_configuration",
            "fixtures/psp_oracle/probe.c": "synthetic_fixture",
            "tools/test_gen_nidnames.py": "synthetic_fixture",
            "assets/titles/synthetic.json": "public_factual_metadata",
        }
        for path, expected in cases.items():
            with self.subTest(path=path):
                self.assertEqual(provenance_ledger._class_for(path, None)[0], expected, path)

    def test_explicit_unresolved_record_stays_unresolved(self) -> None:
        record = {"id": "PROV-X", "classification": "unresolved", "evidence_tier": "N"}
        cls, evidence = provenance_ledger._class_for("src/rt/thing.c", record)
        self.assertEqual(cls, "unresolved")
        self.assertEqual(evidence["record_id"], "PROV-X")
        self.assertIn("reason", evidence)

    def test_specific_record_wins_over_deterministic_rules(self) -> None:
        record = {
            "id": "PROV-UPSTREAM", "classification": "upstream-third-party",
            "evidence_tier": "S", "upstream": "ffmpeg", "upstream_license": "LGPL-2.1-or-later",
        }
        cls, evidence = provenance_ledger._class_for("docs/whatever.md", record)
        self.assertEqual(cls, "upstream_derived")
        self.assertEqual(evidence["record_id"], "PROV-UPSTREAM")

    def test_is_implementation_path(self) -> None:
        for impl in ("src/rt/x.c", "tools/x.py", "hst.ps1", "copy_build_assets.ps1",
                     "interface/src/lib/x.ts", "interface/scripts/prepare-standalone.mjs"):
            with self.subTest(impl=impl):
                self.assertTrue(provenance_ledger.is_implementation_path(impl), impl)
        for not_impl in ("docs/x.md", "Makefile", "mk/build_common.mk", "assets/titles/x.json",
                         ".gitignore", "interface/package.json"):
            with self.subTest(not_impl=not_impl):
                self.assertFalse(provenance_ledger.is_implementation_path(not_impl), not_impl)

    # -- wildcard semantics --------------------------------------------------

    def test_historical_tools_wildcard_is_never_expanded(self) -> None:
        """A `tools/*` record must not cover any path, old or new."""
        detailed = {"records": [
            {"id": "tooling-general", "classification": "project-authored-independent",
             "evidence_tier": "S", "paths": ["tools/*"]},
            {"id": "PROV-SPECIFIC", "classification": "project-authored-independent",
             "evidence_tier": "S", "paths": ["tools/specific_tool.py"]},
        ]}
        with tempfile.TemporaryDirectory() as temp_dir:
            ledger = Path(temp_dir) / "detailed.json"
            ledger.write_text(json.dumps(detailed), encoding="utf-8")
            records = provenance_ledger._implementation_records(ledger)
        self.assertEqual(set(records), {"tools/specific_tool.py"})
        new_tool = "tools/brand_new_tool.py"
        self.assertIsNone(records.get(new_tool))
        cls, evidence = provenance_ledger._class_for(new_tool, records.get(new_tool))
        self.assertEqual(cls, "unresolved")
        self.assertEqual(evidence["source"], "missing path-specific provenance record")

    def test_missing_detailed_ledger_is_empty_not_synthesized(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            records = provenance_ledger._implementation_records(Path(temp_dir) / "absent.json")
        self.assertEqual(records, {})

    def test_missing_detailed_ledger_refuses_generation(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "ledger.json"
            with self.assertRaises(RuntimeError):
                provenance_ledger.build_ledger(
                    output, implementation_ledger=Path(temp_dir) / "absent.json"
                )
            self.assertFalse(output.exists())

    # -- hermetic generator --------------------------------------------------

    def _hermetic_repo(self, files: dict[str, str], include: list[str]) -> tuple[Path, Path]:
        """A tiny real Git repo with a synthetic policy and tracked files."""
        repo = Path(self.enterContext(tempfile.TemporaryDirectory())) / "repo"
        repo.mkdir()
        for argv in (("init", "-q", "."), ("config", "user.email", "t@example.invalid"),
                     ("config", "user.name", "test")):
            subprocess.run(["git", *argv], cwd=repo, check=True, capture_output=True)
        policy = repo / "assets" / "public_source_profile.json"
        policy.parent.mkdir(parents=True, exist_ok=True)
        policy.write_text(
            json.dumps({"name": "hermetic-profile", "include_paths": sorted(include)}),
            encoding="utf-8",
        )
        for rel, text in files.items():
            target = repo / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8")
        subprocess.run(["git", "add", "-A"], cwd=repo, check=True, capture_output=True)
        subprocess.run(["git", "commit", "-qm", "init"], cwd=repo, check=True, capture_output=True)
        return repo, policy

    def _detailed_ledger(self, repo: Path, records: list[dict]) -> Path:
        detailed = repo.parent / "detailed-implementation-ledger.json"
        detailed.write_text(json.dumps({"records": records}), encoding="utf-8")
        return detailed

    def _run_build_ledger(self, repo: Path, policy: Path, detailed: Path, output: Path) -> dict:
        with mock.patch.object(provenance_ledger, "ROOT", repo), \
             mock.patch.object(provenance_ledger, "POLICY_PATH", policy):
            return provenance_ledger.build_ledger(output, implementation_ledger=detailed)

    def test_generation_refuses_unrecorded_implementation_path(self) -> None:
        repo, policy = self._hermetic_repo(
            {"src/new_widget.c": "// synthetic fixture\nint x(void){return 0;}\n",
             "tools/helper.py": "# synthetic fixture\n",
             "docs/guide.md": "# synthetic fixture\n"},
            ["src/new_widget.c", "tools/helper.py", "docs/guide.md"],
        )
        detailed = self._detailed_ledger(repo, [
            {"id": "PROV-HELPER", "classification": "project-authored-independent",
             "evidence_tier": "S", "paths": ["tools/helper.py"]},
        ])
        output = repo.parent / "out" / "public_provenance_ledger.json"
        with self.assertRaises(RuntimeError) as ctx:
            self._run_build_ledger(repo, policy, detailed, output)
        self.assertIn("src/new_widget.c", str(ctx.exception))
        self.assertFalse(output.exists())

    def test_policy_edit_plus_regeneration_cannot_self_authorize(self) -> None:
        """Adding a source to the policy and regenerating must fail without a record."""
        repo, policy = self._hermetic_repo(
            {"src/new_widget.c": "// synthetic fixture\n"}, ["src/new_widget.c"]
        )
        detailed = self._detailed_ledger(repo, [])
        output = repo.parent / "out" / "public_provenance_ledger.json"
        with self.assertRaises(RuntimeError) as ctx:
            self._run_build_ledger(repo, policy, detailed, output)
        self.assertIn("src/new_widget.c", str(ctx.exception))
        self.assertFalse(output.exists())

    def test_generation_refuses_explicit_unresolved_record(self) -> None:
        repo, policy = self._hermetic_repo(
            {"src/thing.c": "// synthetic fixture\n"}, ["src/thing.c"]
        )
        detailed = self._detailed_ledger(repo, [
            {"id": "PROV-UNRESOLVED", "classification": "unresolved",
             "evidence_tier": "N", "paths": ["src/thing.c"]},
        ])
        output = repo.parent / "out" / "public_provenance_ledger.json"
        with self.assertRaises(RuntimeError) as ctx:
            self._run_build_ledger(repo, policy, detailed, output)
        self.assertIn("src/thing.c", str(ctx.exception))

    def test_regeneration_is_deterministic_and_byte_identical(self) -> None:
        repo, policy = self._hermetic_repo(
            {"src/new_widget.c": "// synthetic fixture\nint x(void){return 0;}\n",
             "tools/helper.py": "# synthetic fixture\n",
             "docs/guide.md": "# synthetic fixture\n",
             "Makefile": "all:\n\t@echo hi\n"},
            ["src/new_widget.c", "tools/helper.py", "docs/guide.md", "Makefile"],
        )
        detailed = self._detailed_ledger(repo, [
            {"id": "PROV-WIDGET", "classification": "project-authored-independent",
             "evidence_tier": "S", "paths": ["src/new_widget.c"]},
            {"id": "PROV-HELPER", "classification": "project-authored-independent",
             "evidence_tier": "S", "paths": ["tools/helper.py"]},
        ])
        out1 = repo.parent / "out1" / "public_provenance_ledger.json"
        out2 = repo.parent / "out2" / "public_provenance_ledger.json"
        self._run_build_ledger(repo, policy, detailed, out1)
        self._run_build_ledger(repo, policy, detailed, out2)
        self.assertEqual(out1.read_bytes(), out2.read_bytes())
        document = json.loads(out1.read_text(encoding="utf-8"))
        by_path = {e["path"]: e for e in document["entries"]}
        self.assertEqual(by_path["src/new_widget.c"]["classification"], "project_authored_attested")
        self.assertEqual(by_path["tools/helper.py"]["classification"], "project_authored_attested")
        self.assertEqual(by_path["Makefile"]["classification"], "reviewed_configuration")
        self.assertEqual(by_path["docs/guide.md"]["classification"], "reviewed_documentation")
        # Hash the exact bytes that were committed (the index blob), not the
        # working tree copy, which Git may have re-encoded with CRLF on Windows.
        expected_hash = hashlib.sha256(
            "// synthetic fixture\nint x(void){return 0;}\n".encode("utf-8")
        ).hexdigest()
        self.assertEqual(by_path["src/new_widget.c"]["sha256"], expected_hash)

    # -- validation ----------------------------------------------------------

    def test_validate_ledger_rejects_stale_or_missing_hashes(self) -> None:
        document = {"entries": [
            {"path": "src/a.c", "classification": "project_authored_attested",
             "evidence": {"source": "x"}, "sha256": "too-short"},
            {"path": "src/b.c", "classification": "project_authored_attested",
             "evidence": {"source": "x"}},
        ]}
        errors = provenance_ledger.validate_ledger(document)
        hash_errors = [e for e in errors if "content hash" in e]
        self.assertEqual(len(hash_errors), 2)

    def test_validate_ledger_rejects_unknown_classification(self) -> None:
        document = {"entries": [
            {"path": "src/a.c", "classification": "project-authored-independent",
             "evidence": {"source": "x"}, "sha256": "0" * 64},
        ]}
        errors = provenance_ledger.validate_ledger(document)
        self.assertTrue(any("unsupported provenance class" in e for e in errors))

    def test_validate_ledger_require_resolved_rejects_unresolved(self) -> None:
        document = {"entries": [
            {"path": "src/a.c", "classification": "unresolved",
             "evidence": {"source": "missing path-specific provenance record"}, "sha256": "0" * 64},
        ]}
        self.assertTrue(any(
            "unresolved" in e
            for e in provenance_ledger.validate_ledger(document, require_resolved=True)
        ))
        self.assertFalse(any(
            "unresolved" in e for e in provenance_ledger.validate_ledger(document)
        ))

    def test_check_mode_rejects_unresolved_ledger_via_cli(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            ledger = Path(temp_dir) / "public_provenance_ledger.json"
            ledger.write_text(json.dumps({"entries": [
                {"path": "src/a.c", "classification": "unresolved",
                 "evidence": {"source": "missing path-specific provenance record"},
                 "sha256": "0" * 64},
            ]}), encoding="utf-8")
            code = provenance_ledger.main(["--check", "--output", str(ledger)])
        self.assertEqual(code, 1)


class SelfReferentialEntryTests(unittest.TestCase):
    """The two entries that describe the ledger machinery itself carry no digest.

    `assets/public_provenance_ledger.json` records a `sha256` for every path it
    covers, and two of those paths are the ledger and the export document. A file
    cannot contain its own hash, and the two documents also hash each other:
    `PUBLIC_EXPORT.json` records `provenance_ledger_sha256`. Writing a digest for
    either one is therefore not merely stale, it cannot converge -- refreshing one
    invalidates the other, forever.

    `publish_audit` does not enforce these two paths, so a wrong value here is
    invisible to every gate. That is exactly why it needs a test: the honest
    representation is the absent key, and nothing else was going to notice.
    """

    SELF_REFERENTIAL = ("PUBLIC_EXPORT.json", "assets/public_provenance_ledger.json")

    def setUp(self) -> None:
        self.ledger = json.loads(
            (ROOT / "assets" / "public_provenance_ledger.json").read_text(encoding="utf-8")
        )
        self.by_path = {entry["path"]: entry for entry in self.ledger["entries"]}

    def test_self_referential_entries_carry_no_sha256(self) -> None:
        for path in self.SELF_REFERENTIAL:
            with self.subTest(path=path):
                self.assertIn(path, self.by_path, "the entry itself must still exist")
                self.assertNotIn(
                    "sha256",
                    self.by_path[path],
                    f"{path} records a digest of itself, which can never be correct: "
                    "refreshing it changes the bytes being hashed. Omit the key.",
                )

    def test_every_other_covered_path_does_carry_one(self) -> None:
        """The exemption is exactly two paths, not a general licence to omit."""
        missing = [
            entry["path"]
            for entry in self.ledger["entries"]
            if "sha256" not in entry and entry["path"] not in self.SELF_REFERENTIAL
        ]
        self.assertEqual(missing, [])

    def test_the_exemption_is_justified_by_the_documents_themselves(self) -> None:
        """Pin the circularity, so the exemption cannot be cargo-culted wider.

        Each of the two paths is either the ledger (which would hash itself) or a
        document the ledger's own bytes depend on.
        """
        export = json.loads((ROOT / "PUBLIC_EXPORT.json").read_text(encoding="utf-8"))
        self.assertIn("provenance_ledger_sha256", export)
        ledger_bytes = (ROOT / "assets" / "public_provenance_ledger.json").read_bytes()
        self.assertEqual(
            export["provenance_ledger_sha256"],
            hashlib.sha256(ledger_bytes).hexdigest(),
            "the export must hash the ledger's real bytes; if this drifts the "
            "circularity above is no longer the reason the digests are omitted",
        )


if __name__ == "__main__":
    unittest.main()
