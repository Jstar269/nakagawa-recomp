# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Adversarial regression: the publication gates cannot be bypassed by editing
the checked-in public ledger/policy/export in the same change.

PR #47 made the *generator* fail closed: an implementation-bearing path is only
attested by a path-specific record in the private detailed development ledger,
and the generator refuses to write release evidence while any included path is
unresolved.  The audit-side contract (docs/PUBLICATION_READINESS.md) is:

* a bare ``publish_audit`` run has no externally trusted ledger, so it reports
  ``PROVENANCE_UNVERIFIED`` and fails closed -- the candidate's own checked-in
  ledger is evidence, never an authorization source;
* ``--provenance-self-consistency`` is the explicitly non-attesting developer
  tripwire: coverage, resolution, and content hashes are still enforced against
  the audited ledger itself, but no attestation claim is asserted;
* ``--provenance-ledger <release-controlled-copy>`` is the release anchor: the
  audited ledger must byte-match it, so a candidate that hand-edits its
  checked-in ledger, policy, and export together still cannot self-authorize a
  new implementation path, and relabeling an implementation path as a
  deterministic class (``synthetic_fixture``, ``reviewed_configuration``,
  ``reviewed_documentation``, ``unresolved``) changes the ledger bytes and is
  equally rejected;
* the export records the digest of the ledger blob it was generated with, so a
  ledger edit without a regeneration fails the export cross-digest even in the
  self-consistency tripwire.

These tests prove the audit-side property for every language surface: new C,
Python, PowerShell, TypeScript, fixture probe source, and an added profile path.

Every fixture is synthetic.  No private detailed ledger, key material, or game
data is used or copied.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
AUDIT = TOOLS / "publish_audit.py"

sys.path.insert(0, str(TOOLS))
import publication_policy  # noqa: E402
from public_export import build_document, write_document  # noqa: E402

REQUIRED_FILES = {
    "LICENSE": "synthetic fixture license placeholder\n",
    "NOTICE.md": "# Notices\n\nSynthetic fixture.\n",
    "README.md": "# Synthetic fixture\n",
    "AGENTS.md": "# Synthetic fixture\n",
}

SPDX_C = (
    "// SPDX-License-Identifier: GPL-2.0-or-later\n"
    "/* synthetic fixture - not a real implementation */\n"
    "int synthetic_stub(void) { return 0; }\n"
)
SPDX_PY = (
    "# SPDX-License-Identifier: GPL-2.0-or-later\n"
    "# synthetic fixture - not a real implementation\n"
    "def synthetic_stub():\n    return 0\n"
)
SPDX_TS = (
    "// SPDX-License-Identifier: GPL-2.0-or-later\n"
    "// synthetic fixture - not a real implementation\n"
    "export function syntheticStub(): number { return 0; }\n"
)
SPDX_PS1 = (
    "# SPDX-License-Identifier: GPL-2.0-or-later\n"
    "# synthetic fixture - not a real implementation\n"
    "Write-Output 'synthetic stub'\n"
)

CONTROL_PATHS = {
    "PUBLIC_EXPORT.json",
    "assets/public_source_profile.json",
    "assets/public_provenance_ledger.json",
}

#: Evidence shape produced by the generator for a path-specific detailed-ledger
#: record (see provenance_ledger._class_for).  `record_id` is what makes the
#: claim machine-comparable.
def _attestation_evidence(record_id: str) -> dict:
    return {
        "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
        "record_id": record_id,
        "evidence_tier": "H",
        "authorship": "independent implementation record",
        "upstream_attribution": None,
    }


class _AttestationRepo:
    """A tiny real Git repo with an authoritative branch and a consistent
    policy/ledger/export baseline, mirroring the repository's release layout."""

    def __init__(self, testcase: unittest.TestCase) -> None:
        self.testcase = testcase
        self.tmp = Path(testcase.enterContext(tempfile.TemporaryDirectory()))
        self.repo = self.tmp / "repo"
        self.repo.mkdir()
        for argv in (("init", "-q", "."), ("config", "user.email", "t@example.invalid"),
                     ("config", "user.name", "test")):
            subprocess.run(["git", *argv], cwd=self.repo, check=True, capture_output=True)
        self.content: dict[str, str] = dict(REQUIRED_FILES)
        self.content.update({
            "docs/guide.md": "# Guide\n\nSynthetic fixture.\n",
            "fixtures/legit_probe/probe.c": SPDX_C,
            "src/rt/existing.c": SPDX_C,
            "tools/helper.py": SPDX_PY,
        })
        self.content["assets/release_manifest.json"] = (
            json.dumps({"name": "synthetic", "components": []}, indent=2) + "\n"
        )
        # Placeholder so the policy includes the export from the start; the real
        # bytes are written by ``_write_export``.
        self.content["PUBLIC_EXPORT.json"] = ""
        for rel, text in self.content.items():
            self._write(rel, text)
        self.ledger_entries: list[dict] = []
        self._baseline_commit: str | None = None

    # -- helpers ------------------------------------------------------------

    def _write(self, rel: str, text: str) -> None:
        target = self.repo / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8", newline="\n")
        self.content[rel] = text

    def _sha256(self, rel: str) -> str:
        return hashlib.sha256((self.repo / rel).read_bytes()).hexdigest()

    def _git(self, *argv: str) -> str:
        return subprocess.run(["git", *argv], cwd=self.repo, check=True,
                              capture_output=True, text=True).stdout.strip()

    def _stage(self, extra_files: dict[str, str] | None = None) -> None:
        """Write (optional) new files, update the policy + ledger + export to a
        consistent state, and stage everything -- the exact mutation an
        attacker (or a legitimate contributor) would make in one change."""
        if extra_files:
            for rel, text in extra_files.items():
                self._write(rel, text)
        self._write_policy()
        self._write_ledger()
        self._write_export()
        subprocess.run(["git", "add", "-A"], cwd=self.repo, check=True, capture_output=True)

    # -- baseline construction ----------------------------------------------

    def _write_policy(self) -> None:
        document = {
            "name": "public-safe-v1",
            "profile_version": "2.0.0",
            "min_tool_version": "0.4.0",
            "build_mode": "PUBLIC_SAFE=1",
            "default_disposition": "REJECT",
            "exclude_prefixes": [],
            "exclude_globs": [],
            "exclude_paths": [],
            "include_paths": sorted(set(self.content) | CONTROL_PATHS),
        }
        self._write("assets/public_source_profile.json",
                    json.dumps(document, indent=2) + "\n")

    def _write_ledger(self) -> None:
        """Regenerate ledger metadata while preserving attacker-authored records.

        Entries the test pre-appended to ``self.ledger_entries`` (forged claims)
        are kept verbatim; entries for other tracked files are (re)generated
        from content, exactly like the release process's metadata refresh.
        """
        known = {entry["path"] for entry in self.ledger_entries}
        for rel in sorted(self.content):
            if rel in known or rel in ("assets/public_provenance_ledger.json", "PUBLIC_EXPORT.json"):
                continue
            raw = (self.repo / rel).read_bytes()
            self.ledger_entries.append({
                "path": rel,
                "classification": self._classification_for(rel),
                "evidence": self._evidence_for(rel),
                "sha256": hashlib.sha256(raw).hexdigest(),
            })
        if "assets/public_provenance_ledger.json" not in known:
            self.ledger_entries.append({
                "path": "assets/public_provenance_ledger.json",
                "classification": "reviewed_configuration",
                "evidence": {"source": "synthetic publication fixture"},
            })
        if "PUBLIC_EXPORT.json" not in known:
            self.ledger_entries.append({
                "path": "PUBLIC_EXPORT.json",
                "classification": "generated_from_public_source",
                "evidence": {"source": "synthetic export generator"},
            })
        for entry in self.ledger_entries:
            path = entry.get("path")
            if path in ("assets/public_provenance_ledger.json", "PUBLIC_EXPORT.json") or path not in self.content:
                continue
            entry["sha256"] = hashlib.sha256((self.repo / path).read_bytes()).hexdigest()
        entries = sorted(self.ledger_entries, key=lambda e: e["path"])
        self._write("assets/public_provenance_ledger.json",
                    json.dumps({"schema_version": 1, "entries": entries}, indent=2) + "\n")

    def _classification_for(self, rel: str) -> str:
        if rel.startswith("docs/"):
            return "reviewed_documentation"
        if rel.startswith("fixtures/") or rel.startswith("tools/test_"):
            return "synthetic_fixture"
        if rel.endswith((".json", ".yaml", ".yml")):
            return "reviewed_configuration"
        return "project_authored_attested"

    def _evidence_for(self, rel: str) -> dict:
        if rel.startswith(("docs/", "assets/")):
            return {"source": "synthetic publication fixture"}
        if rel.startswith("fixtures/") or rel.startswith("tools/test_"):
            return {"source": "path-reviewed fixture/test census",
                    "statement": "fixture or test data is synthetic and contains no retail bytes"}
        record_id = {
            "src/rt/existing.c": "PROV-EXISTING",
            "tools/helper.py": "PROV-HELPER",
        }.get(rel, "PROV-SYNTHETIC")
        return _attestation_evidence(record_id)

    def _write_export(self) -> None:
        files = [(rel, (self.repo / rel).read_bytes()) for rel in sorted(self.content)
                 if rel != "PUBLIC_EXPORT.json"]
        files.append(("PUBLIC_EXPORT.json", b""))
        policy = publication_policy.load_policy(self.repo / "assets" / "public_source_profile.json")
        ledger_bytes = (self.repo / "assets" / "public_provenance_ledger.json").read_bytes()
        manifest_bytes = (self.repo / "assets" / "release_manifest.json").read_bytes()
        document = build_document(
            policy, files,
            provenance_ledger=ledger_bytes,
            manifest=manifest_bytes,
        )
        self._write("PUBLIC_EXPORT.json",
                    json.dumps(document, indent=2, ensure_ascii=False) + "\n")

    def build_baseline(self) -> None:
        self._stage()
        subprocess.run(["git", "commit", "-qm", "authoritative baseline"], cwd=self.repo,
                       check=True, capture_output=True)
        self._baseline_commit = self._git("rev-parse", "HEAD")
        subprocess.run(["git", "update-ref", "refs/remotes/origin/main", self._baseline_commit],
                       cwd=self.repo, check=True, capture_output=True)

    def baseline_ledger_bytes(self) -> bytes:
        assert self._baseline_commit is not None
        return subprocess.run(
            ["git", "show", f"{self._baseline_commit}:assets/public_provenance_ledger.json"],
            cwd=self.repo, check=True, capture_output=True,
        ).stdout

    def trusted_ledger_path(self) -> Path:
        trusted = self.tmp / "trusted-ledger.json"
        trusted.write_bytes(self.baseline_ledger_bytes())
        return trusted

    def run_audit(self, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(AUDIT), "--repo-root", str(self.repo),
             "--tracked-only", "--public-scope", *extra],
            cwd=self.repo, capture_output=True, text=True,
        )

    def findings(self, result: subprocess.CompletedProcess) -> list[tuple[str, str]]:
        parsed = []
        for line in (result.stderr or "").splitlines():
            if ":" in line and line.split(":", 1)[0] in {
                "PROVENANCE_UNVERIFIED", "PROVENANCE_LEDGER_MISMATCH", "PROVENANCE_MISSING",
                "PROVENANCE_LEDGER_MISSING", "PROVENANCE_CONTENT_MISMATCH",
                "PROVENANCE_UNRESOLVED", "POLICY_EXPORT_STALE",
            }:
                code, _, rest = line.partition(":")
                parsed.append((code, rest.strip().split(":", 1)[0].strip()))
        return parsed

    def audit_result(self, *extra: str) -> tuple[int, list[tuple[str, str]]]:
        result = self.run_audit(*extra)
        return result.returncode, self.findings(result)


class TestReleaseAnchorRejectsSelfAuthorization(unittest.TestCase):
    """The complete attack -- new implementation path, policy include, forged
    ledger record and digest, regenerated export, in one change -- fails the
    release anchor (--provenance-ledger byte-match) for every language."""

    def _new_implementation_paths(self) -> dict[str, str]:
        return {
            "src/rt/attacker_smuggle.c": SPDX_C,
            "tools/evil_self_attest.py": SPDX_PY,
            "tools/psp_oracle/evil_runner.ps1": SPDX_PS1,
            "interface/src/lib/attacker_module.ts": SPDX_TS,
        }

    def _stage_forged_attack(self, repo: _AttestationRepo) -> dict[str, str]:
        paths = self._new_implementation_paths()
        for rel in paths:
            repo.ledger_entries.append({
                "path": rel,
                "classification": "project_authored_attested",
                "evidence": _attestation_evidence("attacker-invented-record"),
                "sha256": hashlib.sha256(paths[rel].encode("utf-8")).hexdigest(),
            })
        repo._stage(paths)
        return paths

    def test_bare_audit_fails_closed_without_external_ledger(self):
        """A bare audit cannot attest anything: no external ledger, no claims
        verified, PROVENANCE_UNVERIFIED -- even on a clean tree."""
        repo = _AttestationRepo(self)
        repo.build_baseline()
        rc, findings = repo.audit_result()
        self.assertEqual(rc, 1, "a bare audit must fail closed without a trusted ledger")
        self.assertIn("PROVENANCE_UNVERIFIED", {c for c, _ in findings})

    def test_complete_self_authorization_rejected_at_release_anchor(self):
        repo = _AttestationRepo(self)
        repo.build_baseline()
        paths = self._stage_forged_attack(repo)
        trusted = repo.trusted_ledger_path()
        rc, findings = repo.audit_result("--provenance-ledger", str(trusted))
        self.assertEqual(rc, 1, "the complete self-authorization attack must fail the release anchor")
        self.assertIn("PROVENANCE_LEDGER_MISMATCH", {c for c, _ in findings})
        for rel in paths:
            self.assertIn(rel, {p for c, p in findings if c == "PROVENANCE_MISSING"},
                          f"forged attestation for {rel} must be absent from the trusted ledger")

    def test_laundered_unresolved_rejected_at_release_anchor(self):
        repo = _AttestationRepo(self)
        repo.build_baseline()
        path = "tools/evil_unresolved.py"
        repo.ledger_entries.append({
            "path": path,
            "classification": "unresolved",  # laundering: matches the deterministic class
            "evidence": {"source": "missing path-specific provenance record",
                         "statement": "no path-specific record exists in the detailed implementation ledger"},
            "sha256": hashlib.sha256(SPDX_PY.encode("utf-8")).hexdigest(),
        })
        repo._stage({path: SPDX_PY})
        rc, findings = repo.audit_result("--provenance-ledger", str(repo.trusted_ledger_path()))
        self.assertEqual(rc, 1, "laundered unresolved record must fail the release anchor")

    def test_laundered_fixture_config_doc_relabels_rejected_at_release_anchor(self):
        repo = _AttestationRepo(self)
        repo.build_baseline()
        attacks = {
            "tools/evil_laundered.py": ("synthetic_fixture", {
                "source": "path-reviewed fixture/test census",
                "statement": "fixture or test data is synthetic and contains no retail bytes"}),
            "tools/evil_config.py": ("reviewed_configuration", {
                "source": "configuration review",
                "statement": "configuration or dependency metadata reviewed for public release"}),
            "tools/evil_doc.py": ("reviewed_documentation", {
                "source": "public documentation review",
                "statement": "generic/public documentation; no private operational evidence"}),
        }
        for path, (cls, evidence) in attacks.items():
            repo.ledger_entries.append({
                "path": path, "classification": cls, "evidence": evidence,
                "sha256": hashlib.sha256(SPDX_PY.encode("utf-8")).hexdigest(),
            })
        repo._stage(dict.fromkeys(attacks, SPDX_PY))
        rc, findings = repo.audit_result("--provenance-ledger", str(repo.trusted_ledger_path()))
        self.assertEqual(rc, 1, "relabeling implementation paths must fail the release anchor")

    def test_laundered_attestation_claim_on_fixture_path_rejected_at_release_anchor(self):
        repo = _AttestationRepo(self)
        repo.build_baseline()
        path = "fixtures/evil_claim/probe.c"
        repo.ledger_entries.append({
            "path": path,
            "classification": "project_authored_attested",  # claims a detailed-ledger record
            "evidence": _attestation_evidence("attacker-invented-record"),
            "sha256": hashlib.sha256(SPDX_C.encode("utf-8")).hexdigest(),
        })
        repo._stage({path: SPDX_C})
        rc, findings = repo.audit_result("--provenance-ledger", str(repo.trusted_ledger_path()))
        self.assertEqual(rc, 1, "fixture paths cannot self-attest a detailed-ledger claim")

    def test_clean_tree_passes_release_anchor(self):
        repo = _AttestationRepo(self)
        repo.build_baseline()
        rc, findings = repo.audit_result("--provenance-ledger", str(repo.trusted_ledger_path()))
        self.assertEqual(rc, 0, f"clean tree must pass with the trusted ledger; findings: {findings}")
        self.assertEqual(findings, [])

    def test_hash_refresh_of_already_attested_path_passes_release_anchor(self):
        """Editing an attested file and refreshing its ledger hash is the
        legitimate contributor workflow; the release process regenerates the
        ledger from the private detailed ledger, and the release anchor then
        sees a clean byte match."""
        repo = _AttestationRepo(self)
        repo.build_baseline()
        edited = SPDX_C + "int extra_symbol(void) { return 1; }\n"
        repo._write("src/rt/existing.c", edited)
        for entry in repo.ledger_entries:
            if entry["path"] == "src/rt/existing.c":
                entry["sha256"] = repo._sha256("src/rt/existing.c")
        repo._stage()
        # A release-controlled ledger regenerated from the (private) detailed
        # ledger after the edit would carry the refreshed hash.  Model that by
        # refreshing the trusted copy the same way (same JSON formatting the
        # fixture ledger writer uses, so the byte match is about content).
        trusted_doc = json.loads(repo.baseline_ledger_bytes().decode("utf-8"))
        for entry in trusted_doc["entries"]:
            if entry["path"] == "src/rt/existing.c":
                entry["sha256"] = repo._sha256("src/rt/existing.c")
        trusted = repo.tmp / "trusted-ledger-refreshed.json"
        trusted.write_text(json.dumps(trusted_doc, indent=2) + "\n", encoding="utf-8")
        rc, findings = repo.audit_result("--provenance-ledger", str(trusted))
        self.assertEqual(rc, 0, f"legitimate hash refresh must pass; findings: {findings}")


class TestSelfConsistencyTripwire(unittest.TestCase):
    """--provenance-self-consistency is the explicitly non-attesting developer
    scope: it enforces candidate-internal consistency (coverage, resolution,
    hashes, export cross-digest) and does not assert attestation authenticity."""

    def test_clean_tree_passes_tripwire(self):
        repo = _AttestationRepo(self)
        repo.build_baseline()
        rc, findings = repo.audit_result("--provenance-self-consistency")
        self.assertEqual(rc, 0, f"clean tree must pass the tripwire; findings: {findings}")
        self.assertEqual(findings, [])

    def test_tripwire_rejects_unresolved_records(self):
        """An unresolved record for an included path fails even the tripwire:
        the generator refuses to write release evidence while any included path
        is unresolved, so a checked-in ledger never legitimately carries one."""
        repo = _AttestationRepo(self)
        repo.build_baseline()
        path = "tools/evil_unresolved.py"
        repo.ledger_entries.append({
            "path": path,
            "classification": "unresolved",
            "evidence": {"source": "missing path-specific provenance record",
                         "statement": "no path-specific record exists in the detailed implementation ledger"},
            "sha256": hashlib.sha256(SPDX_PY.encode("utf-8")).hexdigest(),
        })
        repo._stage({path: SPDX_PY})
        rc, findings = repo.audit_result("--provenance-self-consistency")
        self.assertEqual(rc, 1, "unresolved records must fail the tripwire")
        self.assertIn("PROVENANCE_UNRESOLVED", {c for c, _ in findings})

    def test_tripwire_rejects_hash_mismatch(self):
        repo = _AttestationRepo(self)
        repo.build_baseline()
        # Tamper with a file but stage it WITHOUT the ledger/export regeneration
        # (``_stage`` would refresh the hash; bypass it).
        repo._write("src/rt/existing.c", SPDX_C + "int tampered(void) { return 1; }\n")
        subprocess.run(["git", "add", "src/rt/existing.c"],
                       cwd=repo.repo, check=True, capture_output=True)
        rc, findings = repo.audit_result("--provenance-self-consistency")
        self.assertEqual(rc, 1, "stale content hash must fail the tripwire")
        self.assertIn("PROVENANCE_CONTENT_MISMATCH", {c for c, _ in findings})

    def test_tripwire_rejects_ledger_edit_without_export_regeneration(self):
        """The export records the digest of the ledger blob it was generated
        with; a ledger edit that was never followed by a regeneration fails the
        export cross-digest even in the self-consistency scope."""
        repo = _AttestationRepo(self)
        repo.build_baseline()
        doc = json.loads((repo.repo / "assets" / "public_provenance_ledger.json").read_text(encoding="utf-8"))
        doc["schema_version"] = 99  # any byte change that is not regenerated
        (repo.repo / "assets" / "public_provenance_ledger.json").write_text(
            json.dumps(doc, indent=2) + "\n", encoding="utf-8")
        subprocess.run(["git", "add", "assets/public_provenance_ledger.json"],
                       cwd=repo.repo, check=True, capture_output=True)
        rc, findings = repo.audit_result("--provenance-self-consistency")
        self.assertEqual(rc, 1, "ledger edit without export regeneration must fail the tripwire")
        self.assertIn("POLICY_EXPORT_STALE", {c for c, _ in findings})

    def test_tripwire_is_non_attesting_by_design(self):
        """Documented boundary: the tripwire checks candidate-internal
        consistency only, so a fully self-consistent forged claim passes it.
        Attestation is asserted only by the release anchor (the byte-match test
        above); this test pins the contract so it cannot silently drift into
        either extreme."""
        repo = _AttestationRepo(self)
        repo.build_baseline()
        path = "tools/evil_self_attest.py"
        repo.ledger_entries.append({
            "path": path,
            "classification": "project_authored_attested",
            "evidence": _attestation_evidence("attacker-invented-record"),
            "sha256": hashlib.sha256(SPDX_PY.encode("utf-8")).hexdigest(),
        })
        repo._stage({path: SPDX_PY})
        rc, findings = repo.audit_result("--provenance-self-consistency")
        self.assertEqual(rc, 0, "self-consistency tripwire is non-attesting by design")
        # ... and the release anchor rejects exactly this tree (see
        # TestReleaseAnchorRejectsSelfAuthorization for the byte-match tests).

    def test_tripwire_passes_new_deterministic_paths(self):
        repo = _AttestationRepo(self)
        repo.build_baseline()
        new_fixture = "fixtures/extra_probe/extra.c"
        new_doc = "docs/new_guide.md"
        repo.ledger_entries.append({
            "path": new_fixture,
            "classification": "synthetic_fixture",
            "evidence": {"source": "path-reviewed fixture/test census",
                         "statement": "fixture or test data is synthetic and contains no retail bytes"},
            "sha256": hashlib.sha256(SPDX_C.encode()).hexdigest(),
        })
        repo.ledger_entries.append({
            "path": new_doc,
            "classification": "reviewed_documentation",
            "evidence": {"source": "synthetic publication fixture"},
            "sha256": hashlib.sha256(b"# New guide\n").hexdigest(),
        })
        repo._stage({new_fixture: SPDX_C, new_doc: "# New guide\n"})
        rc, findings = repo.audit_result("--provenance-self-consistency")
        self.assertEqual(rc, 0, f"deterministic new paths must pass the tripwire; findings: {findings}")
        self.assertEqual(findings, [])


class TestCandidateAuditBoundary(unittest.TestCase):
    """A materialized candidate carries its own ledger, which is
    candidate-controlled evidence; the candidate audit therefore requires the
    release-controlled ledger and fails closed without it."""

    def test_candidate_audit_requires_explicit_trusted_ledger(self):
        repo = _AttestationRepo(self)
        repo.build_baseline()
        candidate = repo.tmp / "candidate"
        shutil.copytree(repo.repo, candidate, ignore=shutil.ignore_patterns(".git"))
        fixture_policy = repo.repo / "assets" / "public_source_profile.json"
        result = subprocess.run(
            [sys.executable, str(AUDIT), "--candidate-root", str(candidate),
             "--candidate-tree", "--public-scope", "--policy", str(fixture_policy)],
            cwd=ROOT, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("PROVENANCE_UNVERIFIED", result.stderr)
        trusted = repo.trusted_ledger_path()
        result = subprocess.run(
            [sys.executable, str(AUDIT), "--candidate-root", str(candidate),
             "--candidate-tree", "--public-scope", "--policy", str(fixture_policy),
             "--provenance-ledger", str(trusted)],
            cwd=ROOT, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

    def test_candidate_audit_rejects_forged_ledger(self):
        repo = _AttestationRepo(self)
        repo.build_baseline()
        path = "src/rt/new_widget.c"
        repo.ledger_entries.append({
            "path": path,
            "classification": "project_authored_attested",
            "evidence": _attestation_evidence("attacker-invented-record"),
            "sha256": hashlib.sha256(SPDX_C.encode()).hexdigest(),
        })
        repo._stage({path: SPDX_C})
        candidate = repo.tmp / "candidate-forged"
        shutil.copytree(repo.repo, candidate, ignore=shutil.ignore_patterns(".git"))
        fixture_policy = repo.repo / "assets" / "public_source_profile.json"
        result = subprocess.run(
            [sys.executable, str(AUDIT), "--candidate-root", str(candidate),
             "--candidate-tree", "--public-scope", "--policy", str(fixture_policy),
             "--provenance-ledger", str(repo.trusted_ledger_path())],
            cwd=ROOT, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("PROVENANCE_LEDGER_MISMATCH", result.stderr)


if __name__ == "__main__":
    unittest.main()
