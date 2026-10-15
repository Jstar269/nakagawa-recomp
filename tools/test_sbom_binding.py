# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regressions for Python lock snapshot binding in generated SBOMs."""

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import generate_sbom
import verify_sbom

RELEASE_MANIFEST = ROOT / "assets" / "release_manifest.json"
PY_LOCK = ROOT / "tools" / "requirements-lock.txt"
PY_LOCK_ID = "SPDXRef-File-python-requirements-lock"
PY_LOCK_IDENTITY = "tools/requirements-lock.txt"


class BindingFixture(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp_path = Path(self._tmp.name)
        self.lock = self.tmp_path / PY_LOCK_IDENTITY
        self.lock.parent.mkdir(parents=True)
        self.lock.write_bytes(PY_LOCK.read_bytes())

    def parse(self):
        return generate_sbom.parse_lockfiles(self.lock, repo_root=self.tmp_path)

    def generate(self):
        parsed = self.parse()
        manifest = json.loads(RELEASE_MANIFEST.read_text(encoding="utf-8"))
        return generate_sbom.generate_spdx23(
            manifest, parsed["py_packages"],
            lock_files=parsed["lock_files"],
            lock_relationships=parsed["lock_relationships"],
        )

    def verify_with_digests(self, spdx):
        spdx_path = self.tmp_path / "sbom.json"
        spdx_path.write_text(json.dumps(spdx), encoding="utf-8", newline="\n")
        return verify_sbom.verify_sbom_matches(spdx_path, RELEASE_MANIFEST, self.lock)

    def verify(self, spdx):
        errors, _digests = self.verify_with_digests(spdx)
        return errors


class TestStandardsConformantLockBinding(BindingFixture):
    def test_no_custom_root_level_evidence_property(self):
        document = self.generate()
        self.assertNotIn("dependencyLockEvidence", document)
        self.assertEqual(
            set(document) & {"dependencyLockEvidence", "lockEvidence", "lock_evidence"}, set()
        )

    def test_lock_binding_uses_schema_valid_python_file_entry(self):
        document = self.generate()
        entry = next(file for file in document["files"] if file["SPDXID"] == PY_LOCK_ID)
        self.assertEqual(entry["fileName"], PY_LOCK_IDENTITY)
        self.assertEqual(
            entry["checksums"],
            [{"algorithm": "SHA256", "checksumValue": hashlib.sha256(self.lock.read_bytes()).hexdigest()}],
        )
        self.assertEqual(entry["licenseConcluded"], "NOASSERTION")
        self.assertEqual(
            set(entry) - {"SPDXID", "fileName", "checksums", "copyrightText",
                          "licenseConcluded", "licenseInfoInFiles", "comment"}, set()
        )

    def test_lockfile_related_to_root_package_with_dependency_manifest_of(self):
        document = self.generate()
        relationships = {
            (rel["spdxElementId"], rel["relationshipType"], rel["relatedSpdxElement"])
            for rel in document["relationships"]
        }
        self.assertIn((PY_LOCK_ID, "DEPENDENCY_MANIFEST_OF", "SPDXRef-Package-nakagawa-recomp"), relationships)

    def test_generated_document_validates_against_official_spdx23_schema(self):
        schema_path = os.environ.get("SPDX23_SCHEMA_PATH")
        try:
            import jsonschema
        except ImportError:
            self.skipTest("official SPDX 2.3 schema or jsonschema not available")
        if not (schema_path and Path(schema_path).is_file()):
            self.skipTest("official SPDX 2.3 schema or jsonschema not available")
        jsonschema.validate(self.generate(), json.loads(Path(schema_path).read_text(encoding="utf-8")))

    def test_no_machine_local_paths_in_generated_document(self):
        document = self.generate()
        serialized = json.dumps(document)
        self.assertNotIn(str(self.tmp_path), serialized)
        self.assertNotIn(str(self.lock), serialized)
        self.assertNotIn("Temp", serialized.replace("NOASSERTION", ""))
        self.assertEqual([item["fileName"] for item in document["files"]], [PY_LOCK_IDENTITY])

    def test_lock_outside_repo_root_fails_closed(self):
        with tempfile.TemporaryDirectory() as other_root:
            outside = Path(other_root) / "requirements-lock.txt"
            outside.write_bytes(PY_LOCK.read_bytes())
            with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
                generate_sbom.parse_lockfiles(outside, repo_root=self.tmp_path)
        self.assertIn("outside the repository root", str(ctx.exception))


class TestSingleSnapshotNoTOCTOU(BindingFixture):
    def test_parse_and_evidence_share_one_read(self):
        with mock.patch.object(Path, "read_bytes", autospec=True, side_effect=Path.read_bytes) as spy:
            parsed = self.parse()
        reads = [call.args[0] for call in spy.call_args_list]
        self.assertEqual(reads.count(self.lock), 1, reads)
        self.assertEqual(
            parsed["lock_evidence"]["python"]["sha256"],
            hashlib.sha256(PY_LOCK.read_bytes()).hexdigest(),
        )
        self.assertEqual(len(parsed["py_packages"]), 4)

    def test_verification_uses_one_snapshot_for_inventory_and_checksum(self):
        document = self.generate()
        original_read = Path.read_bytes
        counts = {}
        def recording_read(path):
            counts[path] = counts.get(path, 0) + 1
            return original_read(path)
        with mock.patch.object(Path, "read_bytes", autospec=True, side_effect=recording_read):
            errors, _digests = self.verify_with_digests(document)
        self.assertEqual(errors, [], str(errors))
        self.assertEqual(counts.get(self.lock), 1, counts)

    def test_changed_lock_bytes_fail_verification_via_file_entry_checksum(self):
        document = self.generate()
        self.lock.write_text(self.lock.read_text(encoding="utf-8") + "\n# changed bytes\n", encoding="utf-8")
        errors = self.verify(document)
        self.assertTrue(any("lockfile binding evidence mismatch for tools/requirements-lock.txt" in error
                            for error in errors), str(errors))

    def test_missing_files_entries_fail_verification(self):
        document = self.generate()
        del document["files"]
        self.assertTrue(any("no `files` entries" in error for error in self.verify(document)))

    def test_wrong_lockfile_identity_in_files_entry_fails(self):
        document = self.generate()
        document["files"][0]["fileName"] = "somewhere/else/requirements-lock.txt"
        errors = self.verify(document)
        self.assertTrue(any("files entry missing for declared dependency lockfile" in error
                            for error in errors), str(errors))

    def test_missing_dependency_manifest_of_relationship_fails(self):
        document = self.generate()
        document["relationships"] = [
            rel for rel in document["relationships"]
            if not (rel["spdxElementId"] == PY_LOCK_ID
                    and rel["relationshipType"] == "DEPENDENCY_MANIFEST_OF")
        ]
        errors = self.verify(document)
        self.assertTrue(any("missing DEPENDENCY_MANIFEST_OF relationship" in error
                            and PY_LOCK_ID in error for error in errors), str(errors))


class TestExactIdentityVerification(BindingFixture):
    def test_correct_name_with_wrong_version_fails(self):
        document = self.generate()
        package = next(p for p in document["packages"] if p.get("name") == "compiledb")
        package["versionInfo"] = "9.9.9"
        package["externalRefs"][0]["referenceLocator"] = "pkg:pypi/compiledb@9.9.9"
        errors = self.verify(document)
        self.assertTrue(any("compiledb" in error and "missing or under-represented" in error
                            for error in errors), str(errors))

    def test_removing_a_locked_dependency_fails(self):
        document = self.generate()
        document["packages"] = [p for p in document["packages"] if p.get("name") != "compiledb"]
        errors = self.verify(document)
        self.assertTrue(any("pkg:pypi/compiledb@0.10.7" in error
                            and "missing or under-represented" in error for error in errors), str(errors))

    def test_unrelated_package_manager_record_fails_as_extra(self):
        document = self.generate()
        document["packages"].append({
            "SPDXID": "SPDXRef-unrelated", "name": "unrelated", "versionInfo": "1.0",
            "externalRefs": [{"referenceCategory": "PACKAGE-MANAGER", "referenceType": "purl",
                              "referenceLocator": "pkg:pypi/unrelated@1.0"}],
        })
        errors = self.verify(document)
        self.assertTrue(any("Unexpected package-manager package record" in error
                            and "pkg:pypi/unrelated@1.0" in error for error in errors), str(errors))

    def test_over_represented_package_identity_fails(self):
        document = self.generate()
        original = next(p for p in document["packages"] if p.get("name") == "compiledb")
        clone = json.loads(json.dumps(original))
        clone["SPDXID"] = "SPDXRef-duplicate-compiledb"
        document["packages"].append(clone)
        errors = self.verify(document)
        self.assertTrue(any("over-represented" in error and "compiledb" in error for error in errors), str(errors))

    def test_existing_repository_sbom_passes(self):
        parsed = generate_sbom.parse_lockfiles(PY_LOCK, repo_root=ROOT)
        manifest = json.loads(RELEASE_MANIFEST.read_text(encoding="utf-8"))
        document = generate_sbom.generate_spdx23(
            manifest, parsed["py_packages"], lock_files=parsed["lock_files"],
            lock_relationships=parsed["lock_relationships"],
        )
        errors, digests = self.verify_with_digests(document)
        self.assertEqual(errors, [], str(errors))
        self.assertEqual(digests["python"], hashlib.sha256(self.lock.read_bytes()).hexdigest())


class TestCliBinding(unittest.TestCase):
    def test_cli_generation_from_repository_lock_succeeds(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "spdx.json"
            code = generate_sbom.main([
                "--manifest", str(RELEASE_MANIFEST), "--py-lock", str(PY_LOCK),
                "--spdx-out", str(output),
            ])
            self.assertEqual(code, 0)
            self.assertTrue(output.is_file())

    def test_cli_rejects_lock_outside_manifest_declaration(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "spdx.json"
            alternate = Path(directory) / "requirements-lock.txt"
            alternate.write_bytes(PY_LOCK.read_bytes())
            code = generate_sbom.main([
                "--manifest", str(RELEASE_MANIFEST), "--py-lock", str(alternate),
                "--spdx-out", str(output),
            ])
            self.assertEqual(code, 1)
            self.assertFalse(output.exists())

    def test_verify_cli_reports_verified_snapshot_digest(self):
        with tempfile.TemporaryDirectory() as directory:
            sbom = Path(directory) / "spdx.json"
            parsed = generate_sbom.parse_lockfiles(PY_LOCK, repo_root=ROOT)
            manifest = json.loads(RELEASE_MANIFEST.read_text(encoding="utf-8"))
            doc = generate_sbom.generate_spdx23(
                manifest, parsed["py_packages"], lock_files=parsed["lock_files"],
                lock_relationships=parsed["lock_relationships"],
            )
            sbom.write_text(json.dumps(doc), encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                code = verify_sbom.main([
                    "--manifest", str(RELEASE_MANIFEST), "--py-lock", str(PY_LOCK),
                    "--spdx", str(sbom),
                ])
            self.assertEqual(code, 0)
            self.assertIn(hashlib.sha256(PY_LOCK.read_bytes()).hexdigest(), output.getvalue())


class TestDeclaredLockEnforcement(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.manifest_path = self.root / "assets" / "release_manifest.json"
        self.manifest_path.parent.mkdir()
        self.lock = self.root / PY_LOCK_IDENTITY
        self.lock.parent.mkdir()
        self.lock.write_bytes(PY_LOCK.read_bytes())
        self.manifest = json.loads(RELEASE_MANIFEST.read_text(encoding="utf-8"))
        self.manifest["lockfiles"] = {"python": PY_LOCK_IDENTITY}
        self.write_manifest()

    def write_manifest(self):
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")

    def test_declared_python_lock_succeeds(self):
        self.assertEqual(generate_sbom.resolve_declared_lockfile(self.manifest, self.manifest_path), self.lock.resolve())

    def test_web_lock_declaration_is_rejected(self):
        self.manifest["lockfiles"]["npm"] = "interface/package-lock.json"
        with self.assertRaises(generate_sbom.LockfileParseError):
            generate_sbom.resolve_declared_lockfile(self.manifest, self.manifest_path)

    def test_escaping_lock_declaration_is_rejected(self):
        self.manifest["lockfiles"]["python"] = "../outside/requirements-lock.txt"
        with self.assertRaises(generate_sbom.LockfileParseError):
            generate_sbom.resolve_declared_lockfile(self.manifest, self.manifest_path)

    def test_missing_or_non_string_lock_declaration_is_rejected(self):
        for lockfiles in ({}, {"python": 42}):
            with self.subTest(lockfiles=lockfiles), self.assertRaises(generate_sbom.LockfileParseError):
                generate_sbom.resolve_declared_lockfile({"lockfiles": lockfiles}, self.manifest_path)

    def test_declared_but_missing_lockfile_is_rejected(self):
        self.lock.unlink()
        with self.assertRaises(generate_sbom.LockfileParseError):
            generate_sbom.resolve_declared_lockfile(self.manifest, self.manifest_path)


if __name__ == "__main__":
    unittest.main()
