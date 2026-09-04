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
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from build_public_export import public_safe_excluded_paths  # noqa: E402
import provenance_ledger  # noqa: E402
import publication_policy  # noqa: E402
from public_export import build_document  # noqa: E402
LEDGER = ROOT / "docs" / "provenance" / "IMPLEMENTATION_PROVENANCE.json"
PUBLIC_LEDGER = ROOT / "assets" / "public_provenance_ledger.json"
REFRESH_TOOL = ROOT / "tools" / "provenance_ledger.py"
AUDIT_TOOL = ROOT / "tools" / "publish_audit.py"

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


class _RefreshFixture:
    """Small real Git repository with a trusted baseline ledger."""

    def __init__(self, testcase: unittest.TestCase) -> None:
        self.tmp = Path(testcase.enterContext(tempfile.TemporaryDirectory()))
        self.repo = self.tmp / "candidate"
        self.repo.mkdir()
        for argv in (
            ("init", "-q", "."),
            ("config", "user.email", "refresh-test@example.invalid"),
            ("config", "user.name", "refresh-test"),
        ):
            subprocess.run(["git", *argv], cwd=self.repo, check=True, capture_output=True)

        self._write("LICENSE", "Synthetic fixture license placeholder\n")
        self._write("NOTICE.md", "# Notices\n\nSynthetic fixture.\n")
        self._write("README.md", "# Synthetic refresh fixture\n")
        self._write("AGENTS.md", "# Synthetic refresh fixture\n")
        self._write("docs/guide.md", "# Guide\n\nSynthetic fixture.\n")
        self._write("src/rt/existing.c", self.source)
        self._write("tools/helper.py", self.helper)
        self._write(self.route, self.route_source)
        self._write("assets/release_manifest.json", '{"name": "synthetic", "components": []}\n')
        self._write_policy()
        self._write("assets/public_provenance_ledger.json", "")
        self._write("PUBLIC_EXPORT.json", "")
        self._write_ledger()
        self._write_export()
        subprocess.run(["git", "add", "-A"], cwd=self.repo, check=True, capture_output=True)
        subprocess.run(["git", "commit", "-qm", "synthetic trusted baseline"], cwd=self.repo,
                       check=True, capture_output=True)
        self.baseline = self._git("rev-parse", "HEAD")

        self.trusted_policy = self.tmp / "trusted-policy.json"
        self.trusted_ledger = self.tmp / "trusted-ledger.json"
        self.trusted_manifest = self.tmp / "trusted-manifest.json"
        shutil.copy2(self.repo / "assets/public_source_profile.json", self.trusted_policy)
        shutil.copy2(self.repo / "assets/public_provenance_ledger.json", self.trusted_ledger)
        shutil.copy2(self.repo / "assets/release_manifest.json", self.trusted_manifest)

    def detailed_ledger(self, *, wildcard_only: bool = False) -> Path:
        """External detailed development ledger matching ``_classification()``.

        Refreshing an implementation class requires this authority, because a
        public snapshot alone cannot show that its implementation entries are
        still backed by exact records.  ``wildcard_only`` models the historical
        public tree, where ``tools/helper.py`` was covered only by the inert
        ``tools/*`` pattern.
        """
        helper = (
            {"id": "tooling-general", "classification": "project-authored-independent",
             "evidence_tier": "S", "paths": ["tools/*"]}
            if wildcard_only else
            {"id": "PROV-HELPER", "classification": "project-authored-independent",
             "evidence_tier": "H", "paths": ["tools/helper.py"]}
        )
        path = self.tmp / ("detailed-wildcard.json" if wildcard_only else "detailed-ledger.json")
        path.write_text(json.dumps({"records": [
            helper,
            {"id": "PROV-EXISTING", "classification": "project-authored-independent",
             "evidence_tier": "H", "paths": ["src/rt/existing.c"]},
            {"id": "PROV-ROUTE", "classification": "project-authored-independent",
             "evidence_tier": "H", "paths": [self.route]},
        ]}, indent=2) + "\n", encoding="utf-8")
        return path

    source = (
        "// SPDX-License-Identifier: GPL-2.0-or-later\n"
        "/* synthetic fixture - not a retail or private input */\n"
        "int existing(void) { return 0; }\n"
    )
    helper = (
        "# SPDX-License-Identifier: GPL-2.0-or-later\n"
        "# synthetic fixture - not a retail or private input\n"
        "def helper():\n    return 0\n"
    )
    route = "interface/src/app/api/recompiler/profiles/[id]/export/route.ts"
    route_source = (
        "// SPDX-License-Identifier: GPL-2.0-or-later\n"
        "// synthetic fixture - literal bracketed route path\n"
        "export function route(): number { return 0; }\n"
    )

    def _write(self, relative: str, content: str | bytes) -> None:
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, str):
            path.write_text(content, encoding="utf-8", newline="\n")
        else:
            path.write_bytes(content)

    def _git(self, *argv: str) -> str:
        return subprocess.run(["git", *argv], cwd=self.repo, check=True,
                              capture_output=True, text=True).stdout.strip()

    def _write_policy(self) -> None:
        included = {
            "LICENSE", "NOTICE.md", "README.md", "AGENTS.md", "docs/guide.md",
            "src/rt/existing.c", "tools/helper.py", self.route, "assets/release_manifest.json",
            "assets/public_source_profile.json", "assets/public_provenance_ledger.json",
            "PUBLIC_EXPORT.json",
        }
        policy = {
            "name": "public-safe-v1",
            "profile_version": "2.0.0",
            "min_tool_version": "0.4.0",
            "build_mode": "PUBLIC_SAFE=1",
            "default_disposition": "REJECT",
            "exclude_prefixes": [],
            "exclude_globs": [],
            "exclude_paths": [],
            "include_paths": sorted(included),
        }
        self._write("assets/public_source_profile.json", json.dumps(policy, indent=2) + "\n")

    def _classification(self, relative: str) -> tuple[str, dict]:
        if relative.startswith("docs/") or relative in {"LICENSE", "NOTICE.md", "README.md", "AGENTS.md"}:
            return "reviewed_documentation", {"source": "synthetic publication fixture"}
        if relative.startswith("assets/") or relative == "PUBLIC_EXPORT.json":
            return "reviewed_configuration", {"source": "synthetic publication fixture"}
        record_id = {
            "src/rt/existing.c": "PROV-EXISTING",
            "interface/src/app/api/recompiler/profiles/[id]/export/route.ts": "PROV-ROUTE",
        }.get(relative, "PROV-HELPER")
        return "project_authored_attested", {
            "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
            "record_id": record_id,
            "evidence_tier": "H",
            "authorship": "independent implementation record",
            "upstream_attribution": None,
        }

    def _write_ledger(self) -> None:
        entries: list[dict] = []
        for path in sorted(
            relative.relative_to(self.repo).as_posix()
            for relative in self.repo.rglob("*")
            if relative.is_file()
            and ".git" not in relative.relative_to(self.repo).parts
            and relative.relative_to(self.repo).as_posix()
            not in {"assets/public_provenance_ledger.json", "PUBLIC_EXPORT.json"}
        ):
            classification, evidence = self._classification(path)
            entries.append({
                "path": path,
                "classification": classification,
                "evidence": evidence,
                "sha256": hashlib.sha256((self.repo / path).read_bytes()).hexdigest(),
            })
        entries.extend([
            {
                "path": "assets/public_provenance_ledger.json",
                "classification": "reviewed_configuration",
                "evidence": {"source": "synthetic publication fixture"},
            },
            {
                "path": "PUBLIC_EXPORT.json",
                "classification": "generated_from_public_source",
                "evidence": {"source": "synthetic export generator"},
            },
        ])
        document = {"schema_version": 1, "entries": sorted(entries, key=lambda entry: entry["path"])}
        self._write("assets/public_provenance_ledger.json", json.dumps(document, indent=2) + "\n")

    def _write_export(self) -> None:
        files = []
        for relative in sorted(
            path.relative_to(self.repo).as_posix()
            for path in self.repo.rglob("*")
            if path.is_file() and ".git" not in path.relative_to(self.repo).parts
        ):
            raw = b"" if relative == "PUBLIC_EXPORT.json" else (self.repo / relative).read_bytes()
            files.append((relative, raw))
        policy = publication_policy.load_policy(self.repo / "assets" / "public_source_profile.json")
        ledger = (self.repo / "assets" / "public_provenance_ledger.json").read_bytes()
        manifest = (self.repo / "assets" / "release_manifest.json").read_bytes()
        document = build_document(policy, files, provenance_ledger=ledger, manifest=manifest)
        self._write("PUBLIC_EXPORT.json", json.dumps(document, indent=2) + "\n")

    def commit_change(self, relative: str, content: str, message: str) -> None:
        self._write(relative, content)
        subprocess.run(["git", "add", relative], cwd=self.repo, check=True, capture_output=True)
        subprocess.run(["git", "commit", "-qm", message], cwd=self.repo, check=True, capture_output=True)

    def refresh(
        self,
        *paths: str,
        trusted_ledger: Path | None = None,
        trusted_tree: str | None = None,
        trusted_manifest: Path | None = None,
        trusted_baseline_ledger: Path | None = None,
    ) -> subprocess.CompletedProcess:
        argv = [
            sys.executable, str(REFRESH_TOOL), "refresh-reviewed",
            "--trusted-ledger", str(trusted_ledger or self.trusted_ledger),
            "--candidate-tree", str(self.repo), "--trusted-tree", trusted_tree or self.baseline,
            "--trusted-policy", str(self.trusted_policy),
            "--trusted-manifest", str(trusted_manifest or self.trusted_manifest), "--paths", *paths,
        ]
        if trusted_baseline_ledger is not None:
            argv[argv.index("--paths"):argv.index("--paths")] = [
                "--trusted-baseline-ledger", str(trusted_baseline_ledger),
            ]
        return subprocess.run(argv, cwd=ROOT, capture_output=True, text=True)

    def audit(self, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                sys.executable, str(AUDIT_TOOL), "--candidate-root", str(self.repo),
                "--candidate-tree", "--public-scope", "--policy", str(self.trusted_policy),
                *extra,
            ],
            cwd=ROOT, capture_output=True, text=True,
        )


class ProvenanceRefreshTests(unittest.TestCase):
    def test_existing_attested_path_refreshes_and_audits(self) -> None:
        fixture = _RefreshFixture(self)
        fixture.commit_change("src/rt/existing.c", fixture.source.replace("return 0", "return 1"), "candidate source edit")
        result = fixture.refresh(
            "src/rt/existing.c",
            trusted_ledger=fixture.detailed_ledger(),
            trusted_baseline_ledger=fixture.trusted_ledger,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

        baseline = json.loads(fixture.trusted_ledger.read_text(encoding="utf-8"))
        refreshed = json.loads((fixture.repo / "assets/public_provenance_ledger.json").read_text(encoding="utf-8"))
        old_entries = {entry["path"]: entry for entry in baseline["entries"]}
        new_entries = {entry["path"]: entry for entry in refreshed["entries"]}
        for path, entry in old_entries.items():
            if path != "src/rt/existing.c":
                self.assertEqual(entry, new_entries[path], path)
        self.assertNotEqual(old_entries["src/rt/existing.c"]["sha256"], new_entries["src/rt/existing.c"]["sha256"])
        self.assertEqual(refreshed["refresh"]["candidate_tree"], fixture._git("rev-parse", "HEAD^{tree}"))
        self.assertEqual(refreshed["refresh"]["trusted_tree"], fixture._git("rev-parse", f"{fixture.baseline}^{{tree}}"))
        self.assertEqual(refreshed["refresh"]["refreshed_paths"], ["src/rt/existing.c"])

        export = json.loads((fixture.repo / "PUBLIC_EXPORT.json").read_text(encoding="utf-8"))
        self.assertEqual(export["candidate_tree"], refreshed["refresh"]["candidate_tree"])
        self.assertEqual(export["provenance_ledger_sha256"], hashlib.sha256(
            (fixture.repo / "assets/public_provenance_ledger.json").read_bytes()
        ).hexdigest())

        self.assertEqual(fixture.audit("--provenance-self-consistency").returncode, 0)
        trusted_refreshed = fixture.tmp / "trusted-refreshed.json"
        shutil.copy2(fixture.repo / "assets/public_provenance_ledger.json", trusted_refreshed)
        audited = fixture.audit("--provenance-ledger", str(trusted_refreshed),
                                "--trusted-manifest", str(fixture.trusted_manifest))
        self.assertEqual(audited.returncode, 0, audited.stderr)

    def test_missing_trusted_ledger_fails_before_writing(self) -> None:
        fixture = _RefreshFixture(self)
        before = (fixture.repo / "assets/public_provenance_ledger.json").read_bytes()
        result = fixture.refresh("src/rt/existing.c", trusted_ledger=fixture.tmp / "missing.json")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TRUSTED_INPUT_MISSING", result.stderr)
        self.assertEqual(before, (fixture.repo / "assets/public_provenance_ledger.json").read_bytes())

    def test_candidate_ledger_cannot_be_used_as_trusted_input(self) -> None:
        fixture = _RefreshFixture(self)
        result = fixture.refresh("src/rt/existing.c", trusted_ledger=fixture.repo / "assets" / "public_provenance_ledger.json")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TRUSTED_INPUT_CANDIDATE_CONTROLLED", result.stderr)

    def test_candidate_ledger_edit_cannot_self_authorize_unresolved_path(self) -> None:
        """A candidate ledger edit cannot turn an unresolved path into attested evidence."""
        fixture = _RefreshFixture(self)
        trusted = json.loads(fixture.trusted_ledger.read_text(encoding="utf-8"))
        for entry in trusted["entries"]:
            if entry["path"] == "src/rt/existing.c":
                entry["classification"] = "unresolved"
        fixture.trusted_ledger.write_text(json.dumps(trusted, indent=2) + "\n", encoding="utf-8")

        candidate = json.loads((fixture.repo / "assets" / "public_provenance_ledger.json").read_text(encoding="utf-8"))
        for entry in candidate["entries"]:
            if entry["path"] == "src/rt/existing.c":
                entry["classification"] = "project_authored_attested"
                entry["evidence"] = {"source": "candidate self-authorization"}
        fixture.commit_change(
            "assets/public_provenance_ledger.json",
            json.dumps(candidate, indent=2) + "\n",
            "candidate ledger self-authorization attempt",
        )
        result = fixture.refresh(
            "src/rt/existing.c",
            trusted_ledger=fixture.repo / "assets" / "public_provenance_ledger.json",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TRUSTED_INPUT_CANDIDATE_CONTROLLED", result.stderr)

    def test_candidate_tree_cannot_be_used_as_trusted_tree(self) -> None:
        fixture = _RefreshFixture(self)
        result = fixture.refresh("src/rt/existing.c", trusted_tree=str(fixture.repo))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TRUSTED_TREE_CANDIDATE_CONTROLLED", result.stderr)

    def test_trusted_manifest_must_match_trusted_tree(self) -> None:
        fixture = _RefreshFixture(self)
        forged = fixture.tmp / "forged-manifest.json"
        forged.write_text('{"name": "forged", "components": []}\n', encoding="utf-8")
        result = fixture.refresh("src/rt/existing.c", trusted_manifest=forged)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TRUSTED_MANIFEST_MISMATCH", result.stderr)

    def test_candidate_policy_substitution_fails(self) -> None:
        fixture = _RefreshFixture(self)
        policy = (fixture.repo / "assets/public_source_profile.json").read_text(encoding="utf-8")
        fixture.commit_change("assets/public_source_profile.json", policy.replace('"build_mode": "PUBLIC_SAFE=1"', '"build_mode": "PUBLIC_SAFE=forged"'), "candidate policy substitution")
        result = fixture.refresh("src/rt/existing.c")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CANDIDATE_POLICY_MISMATCH", result.stderr)

    def test_wildcard_and_directory_authorization_fail(self) -> None:
        for path in ("src/*", "src/rt/"):
            with self.subTest(path=path):
                fixture = _RefreshFixture(self)
                result = fixture.refresh(path)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("REFRESH_PATH_NOT_EXACT", result.stderr)

    def test_literal_bracket_path_is_exact(self) -> None:
        fixture = _RefreshFixture(self)
        fixture.commit_change(
            fixture.route,
            fixture.route_source.replace("return 0", "return 1"),
            "candidate literal bracketed route edit",
        )
        result = fixture.refresh(
            fixture.route,
            trusted_ledger=fixture.detailed_ledger(),
            trusted_baseline_ledger=fixture.trusted_ledger,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_new_implementation_path_is_refused(self) -> None:
        fixture = _RefreshFixture(self)
        fixture.commit_change("src/rt/new_widget.c", fixture.source, "candidate new implementation")
        result = fixture.refresh("src/rt/new_widget.c")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("NEW_PATH_REFUSED", result.stderr)

    def test_unrequested_candidate_change_is_stale(self) -> None:
        fixture = _RefreshFixture(self)
        fixture.commit_change("docs/guide.md", "# Changed guide\n", "candidate unrelated edit")
        result = fixture.refresh("src/rt/existing.c")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CANDIDATE_TREE_STALE", result.stderr)

    def test_dirty_candidate_has_no_tree_identity(self) -> None:
        fixture = _RefreshFixture(self)
        fixture._write("src/rt/existing.c", fixture.source.replace("return 0", "return 2"))
        result = fixture.refresh("src/rt/existing.c")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CANDIDATE_TREE_DIRTY", result.stderr)

    def test_unqualified_classification_is_refused(self) -> None:
        fixture = _RefreshFixture(self)
        document = json.loads(fixture.trusted_ledger.read_text(encoding="utf-8"))
        for entry in document["entries"]:
            if entry["path"] == "src/rt/existing.c":
                entry["classification"] = "reviewed_configuration"
        fixture.trusted_ledger.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        result = fixture.refresh("src/rt/existing.c")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TRUSTED_PATH_UNQUALIFIED", result.stderr)

    def test_trusted_snapshot_must_cover_exact_tree(self) -> None:
        fixture = _RefreshFixture(self)
        document = json.loads(fixture.trusted_ledger.read_text(encoding="utf-8"))
        document["entries"] = [entry for entry in document["entries"] if entry["path"] != "src/rt/existing.c"]
        fixture.trusted_ledger.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        result = fixture.refresh("src/rt/existing.c")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TRUSTED_LEDGER_COVERAGE", result.stderr)

    def test_private_boundary_is_rejected(self) -> None:
        fixture = _RefreshFixture(self)
        document = json.loads(fixture.trusted_ledger.read_text(encoding="utf-8"))
        document["entries"].append({
            "path": "private/secret.txt", "classification": "project_authored_attested",
            "evidence": {"source": "synthetic private input"}, "sha256": hashlib.sha256(b"private").hexdigest(),
        })
        fixture.trusted_ledger.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        result = fixture.refresh("src/rt/existing.c")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TRUSTED_LEDGER_COVERAGE", result.stderr)

    def test_external_detailed_ledger_requires_an_exact_record(self) -> None:
        fixture = _RefreshFixture(self)
        detailed = fixture.tmp / "trusted-detailed.json"
        detailed.write_text(json.dumps({
            "schema_version": 1,
            "records": [
                {"id": "PROV-EXISTING", "classification": "project-authored-independent", "evidence_tier": "H", "paths": ["src/rt/existing.c"]},
                {"id": "PROV-HELPER", "classification": "project-authored-independent", "evidence_tier": "H", "paths": ["tools/helper.py"]},
                {"id": "PROV-ROUTE", "classification": "project-authored-independent", "evidence_tier": "H", "paths": [fixture.route]},
            ],
        }, indent=2) + "\n", encoding="utf-8")
        fixture.commit_change("src/rt/existing.c", fixture.source.replace("return 0", "return 3"), "candidate source edit")
        result = fixture.refresh("src/rt/existing.c", trusted_ledger=detailed)
        self.assertEqual(result.returncode, 0, result.stderr)
        refreshed = json.loads((fixture.repo / "assets/public_provenance_ledger.json").read_text(encoding="utf-8"))
        entry = next(entry for entry in refreshed["entries"] if entry["path"] == "src/rt/existing.c")
        self.assertEqual(entry["evidence"]["record_id"], "PROV-EXISTING")

    def test_detailed_ledger_refresh_projects_external_evidence_over_baseline(self) -> None:
        fixture = _RefreshFixture(self)
        detailed = fixture.tmp / "trusted-detailed.json"
        detailed.write_text(json.dumps({
            "schema_version": 1,
            "records": [
                {"id": "PROV-EXISTING", "classification": "project-authored-independent", "evidence_tier": "H", "paths": ["src/rt/existing.c"]},
                {"id": "PROV-HELPER", "classification": "project-authored-independent", "evidence_tier": "H", "paths": ["tools/helper.py"]},
                {"id": "PROV-ROUTE", "classification": "project-authored-independent", "evidence_tier": "H", "paths": [fixture.route]},
            ],
        }, indent=2) + "\n", encoding="utf-8")
        fixture.commit_change("src/rt/existing.c", fixture.source.replace("return 0", "return 4"), "candidate source edit")
        result = fixture.refresh(
            "src/rt/existing.c",
            trusted_ledger=detailed,
            trusted_baseline_ledger=fixture.trusted_ledger,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        refreshed = json.loads((fixture.repo / "assets/public_provenance_ledger.json").read_text(encoding="utf-8"))
        entry = next(entry for entry in refreshed["entries"] if entry["path"] == "src/rt/existing.c")
        self.assertEqual(entry["evidence"], {
            "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
            "record_id": "PROV-EXISTING",
            "evidence_tier": "H",
            "authorship": "independent implementation record",
            "upstream_attribution": None,
        })

    def test_classification_guard_is_load_bearing_mutation(self) -> None:
        """A mutant that removes the class gate must be killed by this test."""
        fixture = _RefreshFixture(self)
        document = json.loads(fixture.trusted_ledger.read_text(encoding="utf-8"))
        for entry in document["entries"]:
            if entry["path"] == "src/rt/existing.c":
                entry["classification"] = "reviewed_configuration"
        fixture.trusted_ledger.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        original = fixture.refresh("src/rt/existing.c")
        self.assertNotEqual(original.returncode, 0)
        self.assertIn("TRUSTED_PATH_UNQUALIFIED", original.stderr)

        mutant_root = fixture.tmp / "mutant-tools"
        (mutant_root / "tools").mkdir(parents=True)
        mutant_source = REFRESH_TOOL.read_text(encoding="utf-8")
        needle = "if classification in REFRESHABLE_CLASSES:"
        self.assertIn(needle, mutant_source)
        mutant_source = mutant_source.replace(needle, "if True:  # mutation removes the class gate", 1)
        (mutant_root / "tools" / "provenance_ledger.py").write_text(mutant_source, encoding="utf-8")
        shutil.copy2(ROOT / "tools" / "public_export.py", mutant_root / "tools" / "public_export.py")
        shutil.copy2(ROOT / "tools" / "publication_policy.py", mutant_root / "tools" / "publication_policy.py")
        mutant = subprocess.run(
            [
                sys.executable, str(mutant_root / "tools" / "provenance_ledger.py"), "refresh-reviewed",
                "--trusted-ledger", str(fixture.trusted_ledger), "--candidate-tree", str(fixture.repo),
                "--trusted-tree", fixture.baseline, "--trusted-policy", str(fixture.trusted_policy),
                "--trusted-manifest", str(fixture.trusted_manifest), "--paths", "src/rt/existing.c",
            ], cwd=ROOT, capture_output=True, text=True,
        )
        self.assertEqual(mutant.returncode, 0, mutant.stderr)

    def test_existing_documentation_path_refreshes_without_private_record(self) -> None:
        fixture = _RefreshFixture(self)
        baseline = json.loads(fixture.trusted_ledger.read_text(encoding="utf-8"))
        fixture.commit_change("docs/guide.md", "# Changed guide\n", "candidate documentation edit")
        result = fixture.refresh("docs/guide.md")
        self.assertEqual(result.returncode, 0, result.stderr)
        refreshed = json.loads((fixture.repo / "assets" / "public_provenance_ledger.json").read_text(encoding="utf-8"))
        old_entries = {entry["path"]: entry for entry in baseline["entries"]}
        new_entries = {entry["path"]: entry for entry in refreshed["entries"]}
        self.assertEqual(new_entries["docs/guide.md"]["classification"], "reviewed_documentation")
        self.assertNotEqual(old_entries["docs/guide.md"]["sha256"], new_entries["docs/guide.md"]["sha256"])
        self.assertEqual(fixture.audit("--provenance-self-consistency").returncode, 0)

    def test_staged_generated_controls_are_replaced_not_trusted(self) -> None:
        fixture = _RefreshFixture(self)
        fixture.commit_change("src/rt/existing.c", fixture.source.replace("return 0", "return 5"), "candidate source edit")
        fixture.commit_change("assets/public_provenance_ledger.json", "{\"candidate\": true}\n", "candidate ledger output edit")
        fixture.commit_change("PUBLIC_EXPORT.json", "{\"candidate\": true}\n", "candidate export output edit")
        result = fixture.refresh(
            "src/rt/existing.c",
            trusted_ledger=fixture.detailed_ledger(),
            trusted_baseline_ledger=fixture.trusted_ledger,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        refreshed = json.loads((fixture.repo / "assets/public_provenance_ledger.json").read_text(encoding="utf-8"))
        self.assertIn("entries", refreshed)
        exported = json.loads((fixture.repo / "PUBLIC_EXPORT.json").read_text(encoding="utf-8"))
        self.assertIn("included_content_sha256", exported)


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


class BaselineReuseTests(unittest.TestCase):
    """A generated public ledger must be reusable as the next trusted baseline.

    ``refresh-reviewed`` records the candidate tree it read *before* writing the
    regenerated ledger and export, so a shipped ledger's ``refresh`` block can
    never name the tree that then contains it.  Rejecting a snapshot on that
    metadata made every generated baseline permanently unusable while adding no
    authority -- a snapshot is bound to a tree by its entry hashes, which these
    tests exercise directly.
    """

    # -- helpers ---------------------------------------------------------
    def _generation_one(self, fixture: "_RefreshFixture") -> tuple[str, Path]:
        """Refresh once, commit the generated outputs, and export the resulting
        ledger as an external snapshot.  Returns ``(trusted ref, snapshot)``."""
        fixture.commit_change(
            "src/rt/existing.c", fixture.source.replace("return 0", "return 1"), "candidate edit")
        result = fixture.refresh(
            "src/rt/existing.c",
            trusted_ledger=fixture.detailed_ledger(),
            trusted_baseline_ledger=fixture.trusted_ledger,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run(["git", "add", "-A"], cwd=fixture.repo, check=True, capture_output=True)
        subprocess.run(["git", "commit", "-qm", "provenance: refresh public metadata"],
                       cwd=fixture.repo, check=True, capture_output=True)
        trusted_ref = fixture._git("rev-parse", "HEAD")
        snapshot = fixture.tmp / "generation-one.json"
        shutil.copy2(fixture.repo / "assets/public_provenance_ledger.json", snapshot)
        return trusted_ref, snapshot

    def _fails(self, result: subprocess.CompletedProcess, code: str) -> None:
        self.assertNotEqual(result.returncode, 0, "expected a fail-closed refusal")
        self.assertIn(code, result.stderr)

    # -- the round trip the old metadata check made impossible ------------
    def test_generated_snapshot_is_reusable_as_the_next_trusted_baseline(self) -> None:
        fixture = _RefreshFixture(self)
        trusted_ref, snapshot = self._generation_one(fixture)

        recorded = json.loads(snapshot.read_text(encoding="utf-8"))["refresh"]
        committed_tree = fixture._git("rev-parse", "HEAD^{tree}")
        self.assertNotEqual(recorded["candidate_tree"], committed_tree,
                            "the recorded candidate tree predates the generated bytes")
        self.assertNotEqual(recorded["trusted_tree"], committed_tree)

        fixture.commit_change(
            "src/rt/existing.c", fixture.source.replace("return 0", "return 2"), "second edit")
        result = fixture.refresh(
            "src/rt/existing.c", trusted_ledger=fixture.detailed_ledger(),
            trusted_tree=trusted_ref, trusted_baseline_ledger=snapshot)
        self.assertEqual(result.returncode, 0, result.stderr)

        refreshed = json.loads(
            (fixture.repo / "assets/public_provenance_ledger.json").read_text(encoding="utf-8"))
        self.assertEqual(
            {entry["path"] for entry in refreshed["entries"]},
            {entry["path"]
             for entry in json.loads(snapshot.read_text(encoding="utf-8"))["entries"]},
        )
        self.assertEqual(refreshed["refresh"]["refreshed_paths"], ["src/rt/existing.c"])

    # -- a snapshot is bound to a tree by content, not by metadata --------
    def test_stale_snapshot_from_another_tree_fails_closed(self) -> None:
        fixture = _RefreshFixture(self)
        trusted_ref, _snapshot = self._generation_one(fixture)
        fixture.commit_change(
            "src/rt/existing.c", fixture.source.replace("return 0", "return 2"), "second edit")
        # fixture.trusted_ledger is the generation-zero snapshot: correct for the
        # original baseline, stale for the tree now under refresh.
        self._fails(
            fixture.refresh("src/rt/existing.c", trusted_ledger=fixture.detailed_ledger(),
                            trusted_tree=trusted_ref,
                            trusted_baseline_ledger=fixture.trusted_ledger),
            "TRUSTED_LEDGER_TREE_MISMATCH")

    def test_snapshot_path_set_must_cover_the_trusted_tree_exactly(self) -> None:
        fixture = _RefreshFixture(self)
        trusted_ref, snapshot = self._generation_one(fixture)
        document = json.loads(snapshot.read_text(encoding="utf-8"))
        document["entries"] = [e for e in document["entries"] if e["path"] != "docs/guide.md"]
        short = fixture.tmp / "short.json"
        short.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        fixture.commit_change(
            "src/rt/existing.c", fixture.source.replace("return 0", "return 2"), "second edit")
        self._fails(
            fixture.refresh("src/rt/existing.c", trusted_ledger=fixture.detailed_ledger(),
                            trusted_tree=trusted_ref, trusted_baseline_ledger=short),
            "TRUSTED_LEDGER_COVERAGE")

    # -- candidate control of any trusted input still fails closed --------
    def test_unrequested_candidate_change_fails_closed(self) -> None:
        fixture = _RefreshFixture(self)
        trusted_ref, snapshot = self._generation_one(fixture)
        fixture.commit_change(
            "tools/helper.py", fixture.helper.replace("return 0", "return 9"), "unrequested")
        fixture.commit_change(
            "src/rt/existing.c", fixture.source.replace("return 0", "return 2"), "second edit")
        self._fails(
            fixture.refresh("src/rt/existing.c", trusted_ledger=fixture.detailed_ledger(),
                            trusted_tree=trusted_ref, trusted_baseline_ledger=snapshot),
            "CANDIDATE_TREE_STALE")

    def test_candidate_edited_ledger_cannot_self_authorize(self) -> None:
        fixture = _RefreshFixture(self)
        trusted_ref, snapshot = self._generation_one(fixture)
        in_tree = fixture.repo / "assets/public_provenance_ledger.json"
        forged = json.loads(in_tree.read_text(encoding="utf-8"))
        for entry in forged["entries"]:
            if entry["path"] == "tools/helper.py":
                entry["classification"] = "upstream_derived"
                entry["evidence"] = {"source": "forged", "record_id": "FORGED"}
        in_tree.write_text(json.dumps(forged, indent=2) + "\n", encoding="utf-8")
        fixture.commit_change(
            "src/rt/existing.c", fixture.source.replace("return 0", "return 2"), "second edit")
        subprocess.run(["git", "add", "-A"], cwd=fixture.repo, check=True, capture_output=True)
        subprocess.run(["git", "commit", "-qm", "forge own ledger"], cwd=fixture.repo,
                       check=True, capture_output=True)

        result = fixture.refresh("src/rt/existing.c", trusted_ledger=fixture.detailed_ledger(),
                                 trusted_tree=trusted_ref, trusted_baseline_ledger=snapshot)
        self.assertEqual(result.returncode, 0, result.stderr)
        produced = {e["path"]: e
                    for e in json.loads(in_tree.read_text(encoding="utf-8"))["entries"]}
        trusted = {e["path"]: e
                   for e in json.loads(snapshot.read_text(encoding="utf-8"))["entries"]}
        self.assertEqual(produced["tools/helper.py"], trusted["tools/helper.py"],
                         "the candidate's own ledger bytes must be discarded, not honoured")
        drifted = [path for path, entry in trusted.items()
                   if path != "src/rt/existing.c" and produced[path] != entry]
        self.assertEqual(drifted, [], "only the requested path may change")

    def test_candidate_substituted_policy_fails_closed(self) -> None:
        fixture = _RefreshFixture(self)
        trusted_ref, snapshot = self._generation_one(fixture)
        policy_path = fixture.repo / "assets/public_source_profile.json"
        policy = json.loads(policy_path.read_text(encoding="utf-8"))
        policy["include_paths"] = sorted(set(policy["include_paths"]) | {"src/rt/evil.c"})
        policy_path.write_text(json.dumps(policy, indent=2) + "\n", encoding="utf-8")
        subprocess.run(["git", "add", "-A"], cwd=fixture.repo, check=True, capture_output=True)
        subprocess.run(["git", "commit", "-qm", "policy substitution"], cwd=fixture.repo,
                       check=True, capture_output=True)
        self._fails(
            fixture.refresh("src/rt/existing.c", trusted_ledger=fixture.detailed_ledger(),
                            trusted_tree=trusted_ref, trusted_baseline_ledger=snapshot),
            "CANDIDATE_POLICY_MISMATCH")

    def test_candidate_substituted_manifest_fails_closed(self) -> None:
        fixture = _RefreshFixture(self)
        trusted_ref, snapshot = self._generation_one(fixture)
        (fixture.repo / "assets/release_manifest.json").write_text(
            '{"name": "substituted", "components": []}\n', encoding="utf-8")
        subprocess.run(["git", "add", "-A"], cwd=fixture.repo, check=True, capture_output=True)
        subprocess.run(["git", "commit", "-qm", "manifest substitution"], cwd=fixture.repo,
                       check=True, capture_output=True)
        self._fails(
            fixture.refresh("src/rt/existing.c", trusted_ledger=fixture.detailed_ledger(),
                            trusted_tree=trusted_ref, trusted_baseline_ledger=snapshot),
            "CANDIDATE_TREE_STALE")

    def test_trusted_input_inside_the_candidate_fails_closed(self) -> None:
        fixture = _RefreshFixture(self)
        trusted_ref, snapshot = self._generation_one(fixture)
        smuggled = fixture.repo / "smuggled-baseline.json"
        shutil.copy2(snapshot, smuggled)
        subprocess.run(["git", "add", "-A"], cwd=fixture.repo, check=True, capture_output=True)
        subprocess.run(["git", "commit", "-qm", "smuggle trusted input"], cwd=fixture.repo,
                       check=True, capture_output=True)
        self._fails(
            fixture.refresh("src/rt/existing.c", trusted_ledger=fixture.detailed_ledger(),
                            trusted_tree=trusted_ref, trusted_baseline_ledger=smuggled),
            "TRUSTED_INPUT_CANDIDATE_CONTROLLED")

    # -- a new implementation path is still refused -----------------------
    def test_new_implementation_path_without_a_record_fails_closed(self) -> None:
        fixture = _RefreshFixture(self)
        trusted_ref, snapshot = self._generation_one(fixture)
        fixture._write("src/rt/newthing.c", "int newthing(void) { return 1; }\n")
        subprocess.run(["git", "add", "-A"], cwd=fixture.repo, check=True, capture_output=True)
        subprocess.run(["git", "commit", "-qm", "new implementation path"], cwd=fixture.repo,
                       check=True, capture_output=True)
        self._fails(
            fixture.refresh("src/rt/newthing.c", trusted_ledger=fixture.detailed_ledger(),
                            trusted_tree=trusted_ref, trusted_baseline_ledger=snapshot),
            "NEW_PATH_REFUSED")

    # -- baseline reuse must not resurrect wildcard authority -------------
    def test_wildcard_backed_snapshot_entry_cannot_reattest_new_bytes(self) -> None:
        """The historical public tree carries entries minted by the removed
        ``tools/*`` expansion.  Reusing such a snapshot must not let those
        entries follow a path onto content they never described."""
        fixture = _RefreshFixture(self)
        trusted_ref, snapshot = self._generation_one(fixture)
        wildcard_only = fixture.detailed_ledger(wildcard_only=True)
        self.assertNotIn(
            "tools/helper.py",
            provenance_ledger._detailed_records(
                json.loads(wildcard_only.read_text(encoding="utf-8"))),
            "the tools/* wildcard must stay inert",
        )
        fixture.commit_change(
            "tools/helper.py", fixture.helper.replace("return 0", "return 9"), "edit helper")
        self._fails(
            fixture.refresh("tools/helper.py", trusted_ledger=wildcard_only,
                            trusted_tree=trusted_ref, trusted_baseline_ledger=snapshot),
            "TRUSTED_PATH_MISSING")

    def test_snapshot_alone_cannot_refresh_an_implementation_path(self) -> None:
        """Without the detailed ledger there is nothing to prove the snapshot's
        implementation class is still backed by an exact record, so the refresh
        must refuse rather than trust the snapshot's own claim."""
        fixture = _RefreshFixture(self)
        trusted_ref, snapshot = self._generation_one(fixture)
        fixture.commit_change(
            "src/rt/existing.c", fixture.source.replace("return 0", "return 2"), "second edit")
        self._fails(
            fixture.refresh("src/rt/existing.c", trusted_ledger=snapshot,
                            trusted_tree=trusted_ref),
            "TRUSTED_RECORD_REQUIRED")

    # -- deterministic classes keep working from a snapshot alone ---------
    def test_deterministic_paths_still_refresh_from_a_snapshot_alone(self) -> None:
        fixture = _RefreshFixture(self)
        trusted_ref, snapshot = self._generation_one(fixture)
        fixture.commit_change("docs/guide.md", "# Guide\n\nRevised synthetic fixture.\n",
                              "documentation edit")
        result = fixture.refresh("docs/guide.md", trusted_ledger=snapshot,
                                 trusted_tree=trusted_ref)
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = {e["path"]: e for e in json.loads(
            (fixture.repo / "assets/public_provenance_ledger.json").read_text(encoding="utf-8")
        )["entries"]}
        self.assertEqual(entries["docs/guide.md"]["sha256"],
                         hashlib.sha256((fixture.repo / "docs/guide.md").read_bytes()).hexdigest())

    def test_snapshot_cannot_relabel_implementation_as_documentation(self) -> None:
        fixture = _RefreshFixture(self)
        trusted_ref, snapshot = self._generation_one(fixture)
        document = json.loads(snapshot.read_text(encoding="utf-8"))
        for entry in document["entries"]:
            if entry["path"] == "src/rt/existing.c":
                entry["classification"] = "reviewed_documentation"
                entry["evidence"] = {"source": "relabelled"}
        relabelled = fixture.tmp / "relabelled.json"
        relabelled.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        fixture.commit_change(
            "src/rt/existing.c", fixture.source.replace("return 0", "return 2"), "second edit")
        self._fails(
            fixture.refresh("src/rt/existing.c", trusted_ledger=fixture.detailed_ledger(),
                            trusted_tree=trusted_ref, trusted_baseline_ledger=relabelled),
            "TRUSTED_PATH_UNQUALIFIED")


if __name__ == "__main__":
    unittest.main()
