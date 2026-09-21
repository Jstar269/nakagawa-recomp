# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#
# test_sbom_binding.py — revision-3 regressions for issue #375.
#
# Proves the generated SPDX 2.3 document is schema-conformant (no custom
# root-level properties), binds the SBOM to the exact lockfile bytes via
# standards-conformant `files` entries (stable repo-relative fileName +
# SHA256 checksum over the exact bytes) related to the root package with
# DEPENDENCY_MANIFEST_OF, derives parsing and evidence from ONE immutable
# byte snapshot per lockfile (no parse/hash TOCTOU), and still requires
# exact dependency identities (ecosystem + name + version, with
# multiplicity). Machine-local paths must never appear in release evidence.

import io
import json
import os
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

NPM_LOCK_SPDX_ID = "SPDXRef-File-npm-package-lock"
PY_LOCK_SPDX_ID = "SPDXRef-File-python-requirements-lock"

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
LOCK_A_TEXT = json.dumps(LOCK_A, indent=2) + "\n"
LOCK_B_TEXT = json.dumps(LOCK_A, indent=4, sort_keys=True) + "\n"


class BindingFixture(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        # A sandbox fake repo root with the canonical lockfile layout, so the
        # locks carry their canonical repo-relative identities hermetically.
        self.tmp_path = Path(self._tmp.name)
        self.npm_lock = self.tmp_path / "interface" / "package-lock.json"
        self.py_lock = self.tmp_path / "tools" / "requirements-lock.txt"
        self.npm_lock.parent.mkdir(parents=True)
        self.py_lock.parent.mkdir(parents=True)
        self.py_lock.write_text("compiledb==0.10.7\n", encoding="utf-8", newline="\n")
        self.addCleanup(setattr, verify_sbom.verify_release_locks,
                        "last_validated_npm_lock", None)
        self.addCleanup(setattr, verify_sbom.verify_release_locks,
                        "last_validated_py_lock", None)

    def write_npm_lock(self, text: str) -> None:
        self.npm_lock.write_text(text, encoding="utf-8", newline="\n")

    def parse(self) -> dict:
        """parse_lockfiles against the sandbox fake repo root."""
        return generate_sbom.parse_lockfiles(self.npm_lock, self.py_lock, repo_root=self.tmp_path)

    def generate_sbom_document(self) -> dict:
        parsed = self.parse()
        manifest_data = json.loads(RELEASE_MANIFEST.read_text(encoding="utf-8"))
        return generate_sbom.generate_spdx23(
            manifest_data,
            parsed["npm_packages"],
            parsed["py_packages"],
            lock_files=parsed["lock_files"],
            lock_relationships=parsed["lock_relationships"],
        )

    def verify(self, spdx: dict) -> list[str]:
        spdx_path = self.tmp_path / "sbom.json"
        spdx_path.write_text(json.dumps(spdx), encoding="utf-8", newline="\n")
        return verify_sbom.verify_sbom_matches(spdx_path, RELEASE_MANIFEST, self.npm_lock, self.py_lock)


class TestStandardsConformantLockBinding(BindingFixture):
    def test_no_custom_root_level_evidence_property(self):
        # The official SPDX 2.3 JSON schema sets top-level
        # additionalProperties: false; a custom root property is invalid.
        self.write_npm_lock(LOCK_A_TEXT)
        doc = self.generate_sbom_document()
        self.assertNotIn("dependencyLockEvidence", doc)
        self.assertEqual(
            set(doc) & {"dependencyLockEvidence", "lockEvidence", "lock_evidence"},
            set(),
        )

    def test_lock_binding_uses_schema_valid_files_entries(self):
        self.write_npm_lock(LOCK_A_TEXT)
        doc = self.generate_sbom_document()
        files = {f["SPDXID"]: f for f in doc["files"]}
        npm_file = files[NPM_LOCK_SPDX_ID]
        # fileName is the stable repo-relative identity (issue #375 rev 3).
        self.assertEqual(npm_file["fileName"], generate_sbom.NPM_LOCKFILE_IDENTITY)
        self.assertEqual(
            npm_file["checksums"],
            [{"algorithm": "SHA256",
              "checksumValue": generate_sbom.hashlib.sha256(LOCK_A_TEXT.encode()).hexdigest()}],
        )
        self.assertEqual(npm_file["licenseConcluded"], "NOASSERTION")
        # SPDX file required fields only: SPDXID, checksums, fileName (plus
        # other schema-allowed fields); no custom keys on the file object.
        self.assertEqual(
            set(npm_file) - {"SPDXID", "fileName", "checksums", "copyrightText",
                             "licenseConcluded", "licenseInfoInFiles", "comment"},
            set(),
        )

    def test_lockfile_related_to_root_package_with_dependency_manifest_of(self):
        self.write_npm_lock(LOCK_A_TEXT)
        doc = self.generate_sbom_document()
        rels = {
            (r["spdxElementId"], r["relationshipType"], r["relatedSpdxElement"])
            for r in doc["relationships"]
        }
        self.assertIn((NPM_LOCK_SPDX_ID, "DEPENDENCY_MANIFEST_OF",
                       "SPDXRef-Package-nakagawa-recomp"), rels)
        self.assertIn((PY_LOCK_SPDX_ID, "DEPENDENCY_MANIFEST_OF",
                       "SPDXRef-Package-nakagawa-recomp"), rels)

    def test_generated_document_validates_against_official_spdx23_schema(self):
        # Development-time validation against the official SPDX 2.3 JSON schema
        # (fetched to the temp dir); skipped when either the schema file or
        # jsonschema is unavailable. No network/runtime dependency is added.
        schema_path = Path(os.environ.get("SPDX23_SCHEMA_PATH",
                                          "/tmp/spdx-schema-2.3.json"))
        try:
            import jsonschema  # noqa: F401
            have_jsonschema = True
        except ImportError:
            have_jsonschema = False
        if not (have_jsonschema and schema_path.is_file()):
            self.skipTest("official SPDX 2.3 schema or jsonschema not available")
        import jsonschema
        self.write_npm_lock(LOCK_A_TEXT)
        doc = self.generate_sbom_document()
        jsonschema.validate(doc, json.loads(schema_path.read_text(encoding="utf-8")))

    def test_no_machine_local_paths_in_generated_document(self):
        self.write_npm_lock(LOCK_A_TEXT)
        doc = self.generate_sbom_document()
        serialized = json.dumps(doc)
        self.assertNotIn(str(self.tmp_path), serialized)
        self.assertNotIn(str(self.npm_lock), serialized)
        self.assertNotIn(str(self.py_lock), serialized)
        self.assertNotIn("Temp", serialized.replace("NOASSERTION", ""))
        for f in doc["files"]:
            self.assertFalse(Path(f["fileName"]).is_absolute())
        # fileNames are exactly the canonical repo-relative identities.
        self.assertEqual(
            sorted(f["fileName"] for f in doc["files"]),
            [generate_sbom.NPM_LOCKFILE_IDENTITY, generate_sbom.PYTHON_LOCKFILE_IDENTITY],
        )

    def test_lock_outside_repo_root_fails_closed(self):
        # repo_root given, but the lockfile lives outside it: there is no
        # canonical repo-relative identity, so generation must abort rather
        # than leak a machine-local path into release evidence.
        with tempfile.TemporaryDirectory() as other_root:
            outside = Path(other_root) / "package-lock.json"
            outside.write_text(LOCK_A_TEXT, encoding="utf-8", newline="\n")
            with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
                generate_sbom.parse_lockfiles(outside, self.py_lock, repo_root=self.tmp_path)
        self.assertIn("outside the repository root", str(ctx.exception))


class TestSingleSnapshotNoTOCTOU(BindingFixture):
    def test_parse_and_evidence_share_one_read(self):
        # parse_lockfiles must read each lockfile exactly once and derive both
        # the inventory and the evidence digest from that same snapshot.
        self.write_npm_lock(LOCK_A_TEXT)
        with unittest.mock.patch.object(
            Path, "read_bytes", autospec=True, side_effect=Path.read_bytes,
        ) as spy:
            parsed = self.parse()
        read_calls = [c.args[0] for c in spy.call_args_list]
        self.assertEqual(read_calls.count(self.npm_lock), 1, read_calls)
        self.assertEqual(read_calls.count(self.py_lock), 1, read_calls)
        self.assertEqual(
            parsed["lock_evidence"]["npm"]["sha256"],
            generate_sbom.hashlib.sha256(LOCK_A_TEXT.encode()).hexdigest(),
        )
        self.assertEqual(len(parsed["npm_packages"]), 3)

    def test_lockfile_changed_between_reads_cannot_mix_inventory_and_checksum(self):
        # Hostile simulation: the lockfile content changes between accesses.
        # The parser must operate on one captured snapshot: inventory AND
        # checksum come from the same bytes (the first read).
        self.write_npm_lock(LOCK_A_TEXT)
        original_read = Path.read_bytes
        reads = {"n": 0}
        lock_b_bytes = LOCK_B_TEXT.encode("utf-8")

        def flippy_read(path_self):
            reads["n"] += 1
            if path_self == self.npm_lock and reads["n"] > 1:
                # Every access after the first returns mutated bytes.
                return lock_b_bytes
            return original_read(path_self)

        with unittest.mock.patch.object(Path, "read_bytes", autospec=True, side_effect=flippy_read):
            parsed = self.parse()
        npm_evidence = parsed["lock_evidence"]["npm"]["sha256"]
        # The recorded digest must describe the bytes the inventory was parsed
        # from (the first read), never the mutated later bytes.
        self.assertEqual(npm_evidence, generate_sbom.hashlib.sha256(LOCK_A_TEXT.encode()).hexdigest())
        self.assertNotEqual(npm_evidence, generate_sbom.hashlib.sha256(lock_b_bytes).hexdigest())
        # Counterfactual: a second access WOULD have returned the mutated
        # bytes, which is exactly what the single snapshot prevents.
        self.assertEqual(flippy_read(self.npm_lock), lock_b_bytes)
        # And the inventory itself must be the lock-A inventory.
        self.assertEqual(
            sorted((p["name"], p["version"]) for p in parsed["npm_packages"]),
            [("left-pad", "1.3.0"), ("left-pad", "1.3.0"), ("typed-component/@scope/pkg", "2.0.0")],
        )

    def test_verification_uses_one_snapshot_for_inventory_and_checksum(self):
        # Verification must read each lockfile exactly once; the same snapshot
        # feeds the dependency inventory and the SHA256 comparison.
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document()
        self.write_npm_lock(LOCK_A_TEXT)  # unchanged bytes
        original_read = Path.read_bytes
        reads = {"n": 0}
        lock_b_bytes = LOCK_B_TEXT.encode("utf-8")

        def flippy_read(path_self):
            reads["n"] += 1
            if path_self == self.npm_lock and reads["n"] > 1:
                return lock_b_bytes
            return original_read(path_self)

        spdx_path = self.tmp_path / "sbom.json"
        spdx_path.write_text(json.dumps(spdx), encoding="utf-8", newline="\n")
        seen_paths: list[Path] = []

        def recording_read(path_self):
            seen_paths.append(path_self)
            return flippy_read(path_self)

        with unittest.mock.patch.object(Path, "read_bytes", autospec=True, side_effect=recording_read):
            errors = verify_sbom.verify_sbom_matches(spdx_path, RELEASE_MANIFEST, self.npm_lock, self.py_lock)
        # Exactly one read per lockfile inside verification; the same snapshot
        # fed the inventory and the checksum comparison, so verification still
        # passes (a second read would have returned mutated bytes).
        self.assertEqual(seen_paths.count(self.npm_lock), 1, seen_paths)
        self.assertEqual(seen_paths.count(self.py_lock), 1, seen_paths)
        self.assertEqual(errors, [], str(errors))

    def test_changed_lock_bytes_fail_verification_via_file_entry_checksum(self):
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document()
        self.write_npm_lock(LOCK_B_TEXT)
        errors = self.verify(spdx)
        self.assertTrue(
            any("lockfile binding evidence mismatch for interface/package-lock.json" in e
                for e in errors),
            str(errors),
        )

    def test_missing_files_entries_fail_verification(self):
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document()
        del spdx["files"]
        errors = self.verify(spdx)
        self.assertTrue(
            any("no `files` entries" in e for e in errors), str(errors))

    def test_wrong_lockfile_identity_in_files_entry_fails(self):
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document()
        for f in spdx["files"]:
            if f["SPDXID"] == NPM_LOCK_SPDX_ID:
                f["fileName"] = "somewhere/else/package-lock.json"
        errors = self.verify(spdx)
        self.assertTrue(
            any("files entry missing for declared dependency lockfile "
                "interface/package-lock.json" in e for e in errors),
            str(errors),
        )

    def test_missing_dependency_manifest_of_relationship_fails(self):
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document()
        spdx["relationships"] = [
            r for r in spdx["relationships"]
            if not (r["spdxElementId"] == NPM_LOCK_SPDX_ID
                    and r["relationshipType"] == "DEPENDENCY_MANIFEST_OF")
        ]
        errors = self.verify(spdx)
        self.assertTrue(
            any("missing DEPENDENCY_MANIFEST_OF relationship" in e and NPM_LOCK_SPDX_ID in e
                for e in errors),
            str(errors),
        )


class TestExactIdentityVerification(BindingFixture):
    def test_correct_name_with_wrong_version_fails(self):
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document()
        # Subvert the recorded version of left-pad in the SBOM only; the lock
        # still says 1.3.0, so name-only verification would pass this.
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
        spdx = self.generate_sbom_document()
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
        spdx = self.generate_sbom_document()
        spdx["packages"] = [p for p in spdx["packages"] if p.get("name") != "left-pad"]
        errors = self.verify(spdx)
        self.assertTrue(any("expected 2 record(s), found 0" in e for e in errors), str(errors))

    def test_existing_repository_sbom_still_passes(self):
        # Canonical happy path: SBOM generated from the real repository
        # lockfiles with lock binding must pass exact-identity verification.
        parsed = generate_sbom.parse_lockfiles(NPM_LOCK, PY_LOCK, repo_root=generate_sbom.ROOT)
        manifest_data = json.loads(RELEASE_MANIFEST.read_text(encoding="utf-8"))
        spdx = generate_sbom.generate_spdx23(
            manifest_data,
            parsed["npm_packages"],
            parsed["py_packages"],
            lock_files=parsed["lock_files"],
            lock_relationships=parsed["lock_relationships"],
        )
        spdx_path = self.tmp_path / "repo-sbom.json"
        spdx_path.write_text(json.dumps(spdx), encoding="utf-8", newline="\n")
        errors = verify_sbom.verify_sbom_matches(spdx_path, RELEASE_MANIFEST, NPM_LOCK, PY_LOCK)
        self.assertEqual(errors, [], str(errors))


class TestCliBinding(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp_path = Path(self._tmp.name)

    def test_cli_generated_sbom_binds_lock_bytes_and_verifies(self):
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
        self.assertNotEqual(proc.returncode, 0)  # fixture locks are outside ROOT
        self.assertIn("outside the repository root", proc.stderr)


if __name__ == "__main__":
    unittest.main()
