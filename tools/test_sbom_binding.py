# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#
# test_sbom_binding.py — revision-2 regressions for issue #375.
#
# Proves the SBOM is bound to the exact lockfile bytes it was generated from
# (a stale SBOM must fail verification even when the dependency inventory is
# semantically unchanged), and that verification requires exact dependency
# identities (ecosystem + name + version, with multiplicity) rather than a
# set of names.

import json
from pathlib import Path
import sys
import tempfile
import unittest
import unittest.mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import generate_sbom
import verify_sbom

RELEASE_MANIFEST = ROOT / "assets" / "release_manifest.json"
NPM_LOCK = ROOT / "interface" / "package-lock.json"
PY_LOCK = ROOT / "tools" / "requirements-lock.txt"

# Lock A: two records, including a duplicate name+version at two install paths
# (so multiplicity is provable) and a nested scoped package.
LOCK_A = {
    "name": "binding-fixture",
    "version": "1.0.0",
    "lockfileVersion": 3,
    "packages": {
        "": {"name": "binding-fixture", "version": "1.0.0"},
        "node_modules/left-pad": {"version": "1.3.0", "license": "MIT"},
        "node_modules/app/node_modules/left-pad": {"version": "1.3.0", "license": "MIT"},
        "node_modules/typed-component/@scope/pkg": {"version": "2.0.0"},
    },
}
# Lock B: different bytes, SAME semantic dependency inventory (same names,
# versions, duplicate paths). Only formatting differs.
LOCK_B_TEXT = json.dumps(LOCK_A, indent=4, sort_keys=True) + "\n"
LOCK_A_TEXT = json.dumps(LOCK_A, indent=2) + "\n"


class BindingFixture(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp_path = Path(self._tmp.name)
        self.npm_lock = self.tmp_path / "package-lock.json"
        self.py_lock = self.tmp_path / "requirements-lock.txt"
        self.py_lock.write_text("compiledb==0.10.7\n", encoding="utf-8", newline="\n")
        self.addCleanup(setattr, verify_sbom.verify_release_locks,
                        "last_validated_npm_lock", None)
        self.addCleanup(setattr, verify_sbom.verify_release_locks,
                        "last_validated_py_lock", None)

    def write_npm_lock(self, text: str) -> None:
        self.npm_lock.write_text(text, encoding="utf-8", newline="\n")

    def parse_inventory(self, npm_lock: Path) -> tuple[list[dict], list[dict]]:
        return (
            generate_sbom.parse_npm_lockfile(npm_lock),
            generate_sbom.parse_python_lockfile(self.py_lock),
        )

    def generate_sbom_document(self, npm_lock: Path) -> dict:
        npm_pkgs, py_pkgs = self.parse_inventory(npm_lock)
        manifest_data = json.loads(RELEASE_MANIFEST.read_text(encoding="utf-8"))
        spdx = generate_sbom.generate_spdx23(manifest_data, npm_pkgs, py_pkgs)
        spdx["dependencyLockEvidence"] = generate_sbom.compute_lock_evidence(
            npm_lock, self.py_lock
        )
        return spdx

    def verify(self, spdx: dict) -> list[str]:
        spdx_path = self.tmp_path / "sbom.json"
        spdx_path.write_text(json.dumps(spdx), encoding="utf-8", newline="\n")
        return verify_sbom.verify_sbom_matches(spdx_path, RELEASE_MANIFEST, self.npm_lock, self.py_lock)


class TestLockEvidenceBinding(BindingFixture):
    def test_generated_document_carries_exact_byte_evidence(self):
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document(self.npm_lock)
        evidence = spdx["dependencyLockEvidence"]
        self.assertEqual(evidence["npm"]["sha256"], generate_sbom.hashlib.sha256(LOCK_A_TEXT.encode()).hexdigest())
        self.assertEqual(evidence["python"]["sha256"], generate_sbom.hashlib.sha256(b"compiledb==0.10.7\n").hexdigest())
        self.assertEqual(evidence["npm"]["size"], len(LOCK_A_TEXT.encode()))

    def test_valid_sbom_from_lock_a_passes_against_lock_a(self):
        self.write_npm_lock(LOCK_A_TEXT)
        errors = self.verify(self.generate_sbom_document(self.npm_lock))
        self.assertEqual(errors, [], str(errors))

    def test_stale_sbom_fails_when_lock_bytes_change_without_semantic_change(self):
        # Generate evidence from lock A...
        self.write_npm_lock(LOCK_A_TEXT)
        stale_spdx = self.generate_sbom_document(self.npm_lock)
        # ...then change only the lockfile bytes (formatting), leaving the
        # dependency inventory semantically identical (lock B).
        self.write_npm_lock(LOCK_B_TEXT)
        self.assertNotEqual(
            generate_sbom.compute_lock_evidence(self.npm_lock, self.py_lock),
            stale_spdx["dependencyLockEvidence"],
            "sanity: different bytes must produce different evidence",
        )
        errors = self.verify(stale_spdx)
        self.assertTrue(
            any("npm lockfile evidence mismatch" in e for e in errors),
            str(errors),
        )

    def test_evidence_mismatch_names_regeneration_remedy(self):
        self.write_npm_lock(LOCK_A_TEXT)
        stale_spdx = self.generate_sbom_document(self.npm_lock)
        self.write_npm_lock(LOCK_B_TEXT)
        errors = self.verify(stale_spdx)
        self.assertTrue(any("regenerate the SBOM" in e for e in errors), str(errors))

    def test_missing_evidence_block_fails(self):
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document(self.npm_lock)
        del spdx["dependencyLockEvidence"]
        errors = self.verify(spdx)
        self.assertTrue(
            any("dependencyLockEvidence missing or not an object" in e for e in errors),
            str(errors),
        )

    def test_evidence_with_unsupported_schema_version_fails(self):
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document(self.npm_lock)
        spdx["dependencyLockEvidence"]["schema_version"] = 999
        errors = self.verify(spdx)
        self.assertTrue(any("unsupported dependencyLockEvidence schema_version" in e for e in errors), str(errors))

    def test_tampered_evidence_digest_fails(self):
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document(self.npm_lock)
        spdx["dependencyLockEvidence"]["npm"]["sha256"] = "0" * 64
        errors = self.verify(spdx)
        self.assertTrue(any("npm lockfile evidence mismatch" in e for e in errors), str(errors))

    def test_python_lock_byte_change_fails_old_sbom(self):
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document(self.npm_lock)
        self.py_lock.write_text("compiledb==0.10.7 # trailing comment changes bytes only\n",
                                encoding="utf-8", newline="\n")
        errors = self.verify(spdx)
        self.assertTrue(any("python lockfile evidence mismatch" in e for e in errors), str(errors))


class TestExactIdentityVerification(BindingFixture):
    def test_correct_name_with_wrong_version_fails(self):
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document(self.npm_lock)
        # Subvert the recorded version of left-pad in the SBOM only; the lock
        # still says 1.3.0, so name-only verification would pass this.
        self.write_npm_lock(LOCK_A_TEXT)  # lock unchanged
        for pkg in spdx["packages"]:
            if pkg.get("name") == "left-pad":
                pkg["versionInfo"] = "9.9.9"
                for ref in pkg.get("externalRefs", []):
                    ref["referenceLocator"] = ref["referenceLocator"].replace("@1.3.0", "@9.9.9")
        errors = self.verify(spdx)
        self.assertTrue(
            any("left-pad" in e and "missing or under-represented" in e for e in errors),
            str(errors),
        )

    def test_removing_one_duplicate_installation_fails(self):
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document(self.npm_lock)
        # Remove exactly one of the two left-pad SPDX records.
        removed = False
        kept: list[dict] = []
        seen_leftpad = 0
        for pkg in spdx["packages"]:
            if pkg.get("name") == "left-pad":
                seen_leftpad += 1
                if seen_leftpad == 2 and not removed:
                    removed = True
                    continue
            kept.append(pkg)
        self.assertTrue(removed, "fixture sanity: a second left-pad record must exist")
        spdx["packages"] = kept
        errors = self.verify(spdx)
        self.assertTrue(
            any("left-pad" in e and "expected 2 record(s), found 1" in e for e in errors),
            str(errors),
        )

    def test_dropping_the_whole_duplicate_identity_fails(self):
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document(self.npm_lock)
        spdx["packages"] = [p for p in spdx["packages"] if p.get("name") != "left-pad"]
        errors = self.verify(spdx)
        self.assertTrue(any("expected 2 record(s), found 0" in e for e in errors), str(errors))

    def test_existing_repository_sbom_still_passes(self):
        # The canonical happy path: SBOM generated from the real repository
        # lockfiles with byte evidence must pass exact-identity verification.
        npm_pkgs = generate_sbom.parse_npm_lockfile(NPM_LOCK)
        py_pkgs = generate_sbom.parse_python_lockfile(PY_LOCK)
        manifest_data = json.loads(RELEASE_MANIFEST.read_text(encoding="utf-8"))
        spdx = generate_sbom.generate_spdx23(manifest_data, npm_pkgs, py_pkgs)
        spdx["dependencyLockEvidence"] = generate_sbom.compute_lock_evidence(NPM_LOCK, PY_LOCK)
        spdx_path = self.tmp_path / "repo-sbom.json"
        spdx_path.write_text(json.dumps(spdx), encoding="utf-8", newline="\n")
        errors = verify_sbom.verify_sbom_matches(spdx_path, RELEASE_MANIFEST, NPM_LOCK, PY_LOCK)
        self.assertEqual(errors, [], str(errors))

    def test_non_purl_package_records_are_not_confused_with_dependency_identities(self):
        # The root package and manifest components carry no purl external ref;
        # they must not satisfy (or corrupt) dependency identity counting.
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document(self.npm_lock)
        root = next(p for p in spdx["packages"] if p["SPDXID"] == "SPDXRef-Package-nakagawa-recomp")
        self.assertEqual(root.get("name"), "nakagawa-recomp")
        errors = self.verify(spdx)
        self.assertEqual(errors, [], str(errors))


class TestCliBindsEvidence(unittest.TestCase):
    """The generator CLI must embed evidence; verify must reject its absence."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp_path = Path(self._tmp.name)

    def test_cli_generated_sbom_carries_evidence_and_verifies(self):
        npm_lock = self.tmp_path / "package-lock.json"
        npm_lock.write_text(LOCK_A_TEXT, encoding="utf-8", newline="\n")
        py_lock = self.tmp_path / "requirements-lock.txt"
        py_lock.write_text("compiledb==0.10.7\n", encoding="utf-8", newline="\n")
        spdx_out = self.tmp_path / "spdx23.json"

        import subprocess
        proc = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "generate_sbom.py"),
             "--manifest", str(RELEASE_MANIFEST),
             "--npm-lock", str(npm_lock), "--py-lock", str(py_lock),
             "--spdx-out", str(spdx_out)],
            capture_output=True, text=True, cwd=ROOT,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        doc = json.loads(spdx_out.read_text(encoding="utf-8"))
        self.assertIn("dependencyLockEvidence", doc)
        self.assertIn("sha256", doc["dependencyLockEvidence"]["npm"])

        # Verification passes while bytes are unchanged...
        errors = verify_sbom.verify_sbom_matches(spdx_out, RELEASE_MANIFEST, npm_lock, py_lock)
        self.assertEqual(errors, [], str(errors))

        # ...and fails after any lock byte change.
        npm_lock.write_text(LOCK_B_TEXT, encoding="utf-8", newline="\n")
        errors = verify_sbom.verify_sbom_matches(spdx_out, RELEASE_MANIFEST, npm_lock, py_lock)
        self.assertTrue(any("npm lockfile evidence mismatch" in e for e in errors), str(errors))


if __name__ == "__main__":
    unittest.main()
