# SPDX-License-Identifier: GPL-3.0-or-later
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

import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
import unittest.mock
import contextlib

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
        self.py_lock.write_text(PY_LOCK.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")
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

    def verify_with_digests(self, spdx: dict) -> tuple[list[str], dict]:
        spdx_path = self.tmp_path / "sbom.json"
        spdx_path.write_text(json.dumps(spdx), encoding="utf-8", newline="\n")
        return verify_sbom.verify_sbom_matches(
            spdx_path, RELEASE_MANIFEST, self.npm_lock, self.py_lock)

    def verify(self, spdx: dict) -> list[str]:
        errors, _digests = self.verify_with_digests(spdx)
        return errors


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
        # Development-time validation against the official SPDX 2.3 JSON
        # schema; point SPDX23_SCHEMA_PATH at the schema file to enable it.
        # Skipped when the schema path or jsonschema is unavailable, so no
        # network or runtime dependency is added (issue #375 revision 3).
        schema_path = os.environ.get("SPDX23_SCHEMA_PATH")
        try:
            import jsonschema  # noqa: F401
            have_jsonschema = True
        except ImportError:
            have_jsonschema = False
        if not (have_jsonschema and schema_path and Path(schema_path).is_file()):
            self.skipTest("official SPDX 2.3 schema or jsonschema not available")
        import jsonschema
        self.write_npm_lock(LOCK_A_TEXT)
        doc = self.generate_sbom_document()
        jsonschema.validate(doc, json.loads(Path(schema_path).read_text(encoding="utf-8")))

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
        read_counts: dict[Path, int] = {}
        lock_b_bytes = LOCK_B_TEXT.encode("utf-8")

        def flippy_read(path_self):
            read_counts[path_self] = read_counts.get(path_self, 0) + 1
            if path_self == self.npm_lock and read_counts[path_self] > 1:
                return lock_b_bytes
            return original_read(path_self)

        spdx_path = self.tmp_path / "sbom.json"
        spdx_path.write_text(json.dumps(spdx), encoding="utf-8", newline="\n")
        seen_paths: list[Path] = []

        def recording_read(path_self):
            seen_paths.append(path_self)
            return flippy_read(path_self)

        with unittest.mock.patch.object(Path, "read_bytes", autospec=True, side_effect=recording_read):
            errors, _digests = verify_sbom.verify_sbom_matches(
                spdx_path, RELEASE_MANIFEST, self.npm_lock, self.py_lock)
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

    def test_over_represented_duplicate_installation_fails(self):
        # A THIRD identical (purl, name, version) record must fail: exact
        # multiplicity, not merely "at least".
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document()
        original = next(p for p in spdx["packages"] if p.get("name") == "left-pad")
        clone = json.loads(json.dumps(original))
        clone["SPDXID"] = "SPDXRef-npm-left-pad-1.3.0-injected"
        spdx["packages"].append(clone)
        errors = self.verify(spdx)
        self.assertTrue(
            any("over-represented" in e and "pkg:npm/left-pad@1.3.0" in e
                and "expected 2 record(s), found 3" in e for e in errors),
            str(errors),
        )

    def test_unrelated_package_manager_record_fails_as_extra(self):
        # A PACKAGE-MANAGER purl record outside the declared lockfiles is an
        # extra and must be rejected; there is no legitimate class of non-lock
        # PACKAGE-MANAGER record in this SBOM contract.
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document()
        spdx["packages"].append({
            "SPDXID": "SPDXRef-npm-unrelated-1.0.0",
            "name": "unrelated",
            "versionInfo": "1.0.0",
            "downloadLocation": "NOASSERTION",
            "filesAnalyzed": False,
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": "NOASSERTION",
            "copyrightText": "NOASSERTION",
            "externalRefs": [{
                "referenceCategory": "PACKAGE-MANAGER",
                "referenceType": "purl",
                "referenceLocator": "pkg:npm/unrelated@1.0.0",
            }],
        })
        errors = self.verify(spdx)
        self.assertTrue(
            any("Unexpected package-manager package record" in e and "pkg:npm/unrelated@1.0.0" in e
                for e in errors),
            str(errors),
        )

    def test_extra_tuple_with_expected_purl_but_wrong_name_fails(self):
        # The exact-Counter contract is full-tuple: an extra PACKAGE-MANAGER
        # record reusing a VALID purl but carrying a wrong name must fail, not
        # escape through purl-only membership.
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document()
        spdx["packages"].append({
            "SPDXID": "SPDXRef-npm-left-pad-wrongname",
            "name": "wrong-name",
            "versionInfo": "9.9.9",
            "downloadLocation": "NOASSERTION",
            "filesAnalyzed": False,
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": "NOASSERTION",
            "copyrightText": "NOASSERTION",
            "externalRefs": [{
                "referenceCategory": "PACKAGE-MANAGER",
                "referenceType": "purl",
                "referenceLocator": "pkg:npm/left-pad@1.3.0",
            }],
        })
        errors = self.verify(spdx)
        self.assertTrue(
            any("Unexpected package-manager package record" in e
                and "pkg:npm/left-pad@1.3.0" in e and "'wrong-name'" in e
                for e in errors),
            str(errors),
        )

    def test_extra_tuple_with_expected_purl_but_wrong_version_fails(self):
        # Same purl, wrong versionInfo: still an unexpected tuple under exact
        # Counter equality.
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document()
        spdx["packages"].append({
            "SPDXID": "SPDXRef-npm-left-pad-wrongversion",
            "name": "left-pad",
            "versionInfo": "9.9.9",
            "downloadLocation": "NOASSERTION",
            "filesAnalyzed": False,
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": "NOASSERTION",
            "copyrightText": "NOASSERTION",
            "externalRefs": [{
                "referenceCategory": "PACKAGE-MANAGER",
                "referenceType": "purl",
                "referenceLocator": "pkg:npm/left-pad@1.3.0",
            }],
        })
        errors = self.verify(spdx)
        self.assertTrue(
            any("Unexpected package-manager package record" in e
                and "pkg:npm/left-pad@1.3.0" in e and "'9.9.9'" in e
                for e in errors),
            str(errors),
        )

    def test_substituted_tuple_with_same_purl_fails(self):
        # An expected tuple is removed and a wrong tuple reusing its purl is
        # substituted: Counter equality must report BOTH the shortfall and
        # the unexpected tuple.
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document()
        kept: list[dict] = []
        removed = False
        for pkg in spdx["packages"]:
            if (not removed and pkg.get("name") == "left-pad"
                    and pkg.get("versionInfo") == "1.3.0"):
                removed = True
                continue
            kept.append(pkg)
        self.assertTrue(removed, "fixture sanity: a left-pad record must exist")
        kept.append({
            "SPDXID": "SPDXRef-npm-left-pad-substituted",
            "name": "wrong-name",
            "versionInfo": "9.9.9",
            "downloadLocation": "NOASSERTION",
            "filesAnalyzed": False,
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": "NOASSERTION",
            "copyrightText": "NOASSERTION",
            "externalRefs": [{
                "referenceCategory": "PACKAGE-MANAGER",
                "referenceType": "purl",
                "referenceLocator": "pkg:npm/left-pad@1.3.0",
            }],
        })
        spdx["packages"] = kept
        errors = self.verify(spdx)
        self.assertTrue(
            any("missing or under-represented" in e and "pkg:npm/left-pad@1.3.0" in e
                for e in errors), str(errors))
        self.assertTrue(
            any("Unexpected package-manager package record" in e
                and "pkg:npm/left-pad@1.3.0" in e and "'wrong-name'" in e
                for e in errors), str(errors))

    def test_exact_counter_passes_and_mutation_fails(self):
        # Contract statement: the generated document's Counter equals the
        # lock-derived Counter exactly (PASS), and a third identical tuple
        # (FAIL) — the two halves of exact Counter equality.
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document()
        self.assertEqual(self.verify(spdx), [])
        clone = json.loads(json.dumps(
            next(p for p in spdx["packages"] if p.get("name") == "left-pad")))
        clone["SPDXID"] = "SPDXRef-npm-left-pad-1.3.0-extra"
        spdx["packages"].append(clone)
        errors = self.verify(spdx)
        self.assertTrue(
            any("over-represented" in e and "pkg:npm/left-pad@1.3.0" in e
                and "expected 2 record(s), found 3" in e for e in errors),
            str(errors),
        )

    def test_verify_sbom_matches_returns_verified_snapshot_digests(self):
        # The returned digests are the snapshots' own sha256 — the exact bytes
        # whose inventory and checksum binding were verified — not a reread.
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document()
        spdx_path = self.tmp_path / "sbom.json"
        spdx_path.write_text(json.dumps(spdx), encoding="utf-8", newline="\n")
        expected_npm = hashlib.sha256(self.npm_lock.read_bytes()).hexdigest()
        expected_py = hashlib.sha256(self.py_lock.read_bytes()).hexdigest()
        errors, digests = verify_sbom.verify_sbom_matches(
            spdx_path, RELEASE_MANIFEST, self.npm_lock, self.py_lock)
        self.assertEqual(errors, [])
        self.assertEqual(digests["npm"], expected_npm)
        self.assertEqual(digests["python"], expected_py)
        # Flip the bytes AFTER verification: the returned digests are
        # unchanged — they are carried, not recomputed.
        self.write_npm_lock(LOCK_B_TEXT)
        self.assertEqual(digests["npm"], expected_npm)

    def test_non_lock_records_are_not_treated_as_dependencies(self):
        # The root package, provenance-family packages, and manifest components
        # carry no PACKAGE-MANAGER purl; they must never count as (or against)
        # lock dependency identities.
        self.write_npm_lock(LOCK_A_TEXT)
        spdx = self.generate_sbom_document()
        purl_records = [p for p in spdx["packages"] if p.get("externalRefs")]
        self.assertTrue(purl_records, "fixture sanity: dependency records exist")
        for pkg in spdx["packages"]:
            if pkg["SPDXID"].startswith(("SPDXRef-Package-", "SPDXRef-comp-", "SPDXRef-family-")):
                self.assertFalse(pkg.get("externalRefs"), pkg["SPDXID"])
        errors = self.verify(spdx)
        self.assertEqual(errors, [], str(errors))

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
        errors, _digests = verify_sbom.verify_sbom_matches(
            spdx_path, RELEASE_MANIFEST, NPM_LOCK, PY_LOCK)
        self.assertEqual(errors, [], str(errors))


class TestCliBinding(unittest.TestCase):
    def test_cli_generation_from_repository_locks_succeeds(self):
        import subprocess
        spdx_out = Path(tempfile.mkdtemp()) / "spdx23.json"
        proc = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "generate_sbom.py"),
             "--spdx-out", str(spdx_out)],
            capture_output=True, text=True, cwd=ROOT,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertTrue(spdx_out.is_file())
        doc = json.loads(spdx_out.read_text(encoding="utf-8"))
        self.assertEqual(
            sorted(f["fileName"] for f in doc["files"]),
            [generate_sbom.NPM_LOCKFILE_IDENTITY, generate_sbom.PYTHON_LOCKFILE_IDENTITY],
        )

    def test_cli_rejects_cli_lock_outside_repo(self):
        import subprocess
        tmp = Path(tempfile.mkdtemp())
        outside_lock = tmp / "package-lock.json"
        outside_lock.write_text(LOCK_A_TEXT, encoding="utf-8", newline="\n")
        proc = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "generate_sbom.py"),
             "--npm-lock", str(outside_lock)],
            capture_output=True, text=True, cwd=ROOT,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("is not the lockfile declared by the release manifest", proc.stderr)

    def test_cli_success_digest_comes_from_verified_snapshot(self):
        # The success message must report the digests of the snapshots that
        # were verified, never a post-verification reread: flip the lockfile
        # bytes after verification and prove the printed digest still
        # describes the verified bytes (and that Path.read_bytes is not
        # called again during reporting).
        import subprocess
        tmp = Path(tempfile.mkdtemp())
        sandbox = tmp / "repo"
        (sandbox / "interface").mkdir(parents=True)
        (sandbox / "tools").mkdir(parents=True)
        (sandbox / "assets").mkdir(parents=True)
        npm_lock = sandbox / "interface" / "package-lock.json"
        py_lock = sandbox / "tools" / "requirements-lock.txt"
        npm_lock.write_text(LOCK_A_TEXT, encoding="utf-8", newline="\n")
        py_lock.write_text(PY_LOCK.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")
        (sandbox / "interface" / "package.json").write_text(
            json.dumps({"devDependencies": {"typescript": "^5.0.0", "eslint": "^9.0.0"}}),
            encoding="utf-8", newline="\n")
        manifest_data = json.loads(RELEASE_MANIFEST.read_text(encoding="utf-8"))
        manifest_data["lockfiles"] = {
            "npm": "interface/package-lock.json",
            "python": "tools/requirements-lock.txt",
        }
        manifest_path = sandbox / "assets" / "release_manifest.json"
        manifest_path.write_text(json.dumps(manifest_data), encoding="utf-8", newline="\n")
        spdx_data = generate_sbom.parse_lockfiles(npm_lock, py_lock, repo_root=sandbox)
        spdx = generate_sbom.generate_spdx23(
            manifest_data, spdx_data["npm_packages"], spdx_data["py_packages"],
            lock_files=spdx_data["lock_files"],
            lock_relationships=spdx_data["lock_relationships"],
        )
        spdx_path = sandbox / "sbom.json"
        spdx_path.write_text(json.dumps(spdx), encoding="utf-8", newline="\n")
        verified_npm_digest = hashlib.sha256(npm_lock.read_bytes()).hexdigest()
        verified_py_digest = hashlib.sha256(py_lock.read_bytes()).hexdigest()

        # Flip the lock bytes BEFORE the run: the snapshot inside verification
        # is captured at its first read, so the digest it verified against is
        # the ORIGINAL bytes only if the report does not reread the file.
        npm_lock.write_text(LOCK_B_TEXT, encoding="utf-8", newline="\n")

        # \< rogue flipped bytes must still fail the files-entry checksum with
        # the ORIGINAL snapshot digest (single-snapshot behavior).
        proc_fail = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "verify_sbom.py"),
             "--manifest", str(manifest_path),
             "--npm-lock", str(npm_lock), "--py-lock", str(py_lock),
             "--spdx", str(spdx_path)],
            capture_output=True, text=True, cwd=ROOT,
        )
        self.assertNotEqual(proc_fail.returncode, 0)
        self.assertIn(verified_npm_digest, proc_fail.stderr)

        # Restore the verified bytes: the flip above exists only for the
        # fail-path check; the success leg below needs the original bytes on
        # disk so the two legitimate reads see them.
        npm_lock.write_text(LOCK_A_TEXT, encoding="utf-8", newline="\n")

        # Now the same-snapshot success path: verification legitimately reads
        # each lockfile exactly twice (once in verify_release_locks, once for
        # the verify_sbom_matches snapshot). Patch Path.read_bytes to record
        # reads and serve FLIPPED (valid, differently formatted, different
        # digest) bytes on any THIRD read of a lockfile. main() must succeed,
        # print exactly the verified-snapshot digests, and never trigger the
        # flip: a third read would mean the report reread the files.
        flipped_npm_digest = hashlib.sha256(LOCK_B_TEXT.encode("utf-8")).hexdigest()
        seen_paths: list[Path] = []
        real_read_bytes = Path.read_bytes

        def flipping_read(path_self):
            seen_paths.append(path_self)
            if path_self in (npm_lock, py_lock) and seen_paths.count(path_self) >= 3:
                return (LOCK_B_TEXT + "\n").encode("utf-8")
            return real_read_bytes(path_self)

        stdout_buffer = io.StringIO()
        with unittest.mock.patch.object(Path, "read_bytes", autospec=True,
                                        side_effect=flipping_read), \
                contextlib.redirect_stdout(stdout_buffer):
            code = verify_sbom.main([
                "--manifest", str(manifest_path),
                "--npm-lock", str(npm_lock), "--py-lock", str(py_lock),
                "--spdx", str(spdx_path),
            ])
        self.assertEqual(code, 0)
        self.assertEqual(seen_paths.count(npm_lock), 2, seen_paths)
        self.assertEqual(seen_paths.count(py_lock), 2, seen_paths)
        output = stdout_buffer.getvalue()
        self.assertIn(verified_npm_digest, output)
        self.assertIn(verified_py_digest, output)
        self.assertNotIn(flipped_npm_digest, output)


class TestDeclaredLockEnforcement(unittest.TestCase):
    """Release generation must use exactly the manifest-declared lockfiles,
    with SPDX fileNames derived from those declarations (issue #375 rev 4)."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp_path = Path(self._tmp.name)

    def _write_repo(self, npm_rel: str, py_rel: str) -> tuple[Path, Path, Path]:
        """Sandbox repo: manifest at assets/, declared locks, alternate locks."""
        npm_lock = self.tmp_path / npm_rel
        py_lock = self.tmp_path / py_rel
        npm_lock.parent.mkdir(parents=True, exist_ok=True)
        py_lock.parent.mkdir(parents=True, exist_ok=True)
        npm_lock.write_text(LOCK_A_TEXT, encoding="utf-8", newline="\n")
        py_lock.write_text(PY_LOCK.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")
        manifest_data = json.loads(RELEASE_MANIFEST.read_text(encoding="utf-8"))
        manifest_data["lockfiles"] = {"npm": npm_rel, "python": py_rel}
        manifest_path = self.tmp_path / "assets" / "release_manifest.json"
        manifest_path.parent.mkdir(parents=True, exist_ok=True)
        manifest_path.write_text(json.dumps(manifest_data), encoding="utf-8", newline="\n")
        return manifest_path, npm_lock, py_lock

    def _run_main(self, manifest_path: Path, npm_lock: Path, py_lock: Path) -> int:
        return generate_sbom.main([
            "--manifest", str(manifest_path),
            "--npm-lock", str(npm_lock),
            "--py-lock", str(py_lock),
            "--spdx-out", str(self.tmp_path / "out" / "spdx23.json"),
        ])

    def test_canonical_manifest_and_locks_succeed(self):
        manifest_path, npm_lock, py_lock = self._write_repo(
            "interface/package-lock.json", "tools/requirements-lock.txt")
        code = self._run_main(manifest_path, npm_lock, py_lock)
        self.assertEqual(code, 0)
        doc = json.loads(
            (self.tmp_path / "out" / "spdx23.json").read_text(encoding="utf-8"))
        self.assertEqual(
            sorted(f["fileName"] for f in doc["files"]),
            [generate_sbom.NPM_LOCKFILE_IDENTITY, generate_sbom.PYTHON_LOCKFILE_IDENTITY],
        )

    def test_repo_contained_alternate_lock_path_is_rejected(self):
        # The alternate lock exists inside the repo but the manifest does not
        # declare it: generation must fail closed even though parsing would work.
        manifest_path, _npm_lock, py_lock = self._write_repo(
            "interface/package-lock.json", "tools/requirements-lock.txt")
        alternate = self.tmp_path / "interface" / "alternate-package-lock.json"
        alternate.write_text(LOCK_A_TEXT, encoding="utf-8", newline="\n")
        code = self._run_main(manifest_path, alternate, py_lock)
        self.assertEqual(code, 1)
        self.assertFalse((self.tmp_path / "out" / "spdx23.json").exists())

    def test_cli_and_manifest_disagreement_is_rejected(self):
        # Both locks exist and are inside the repo, but the CLI points at the
        # alternate copy while the manifest declares the canonical one.
        manifest_path, npm_lock, py_lock = self._write_repo(
            "interface/package-lock.json", "tools/requirements-lock.txt")
        alternate = self.tmp_path / "interface" / "alternate-package-lock.json"
        alternate.write_text(LOCK_A_TEXT, encoding="utf-8", newline="\n")
        code = self._run_main(manifest_path, alternate, py_lock)
        self.assertEqual(code, 1)

    def test_manifest_escaping_lock_declaration_is_rejected(self):
        manifest_path, npm_lock, py_lock = self._write_repo(
            "interface/package-lock.json", "tools/requirements-lock.txt")
        outside = Path(tempfile.mkdtemp()) / "requirements-lock.txt"
        outside.write_text("compiledb==0.10.7\n", encoding="utf-8", newline="\n")
        manifest_data = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest_data["lockfiles"]["python"] = "../evil-requirements-lock.txt"
        manifest_path.write_text(json.dumps(manifest_data), encoding="utf-8", newline="\n")
        # The escaped target must not need to exist for the rejection, but
        # point --py-lock at the outside file to simulate the mismatch too.
        code = self._run_main(manifest_path, npm_lock, outside)
        self.assertEqual(code, 1)

    def test_missing_manifest_lock_declaration_is_rejected(self):
        manifest_path, npm_lock, py_lock = self._write_repo(
            "interface/package-lock.json", "tools/requirements-lock.txt")
        manifest_data = json.loads(manifest_path.read_text(encoding="utf-8"))
        del manifest_data["lockfiles"]["python"]
        manifest_path.write_text(json.dumps(manifest_data), encoding="utf-8", newline="\n")
        code = self._run_main(manifest_path, npm_lock, py_lock)
        self.assertEqual(code, 1)

    def test_non_string_manifest_lock_declaration_is_rejected(self):
        manifest_path, npm_lock, py_lock = self._write_repo(
            "interface/package-lock.json", "tools/requirements-lock.txt")
        manifest_data = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest_data["lockfiles"]["npm"] = 42
        manifest_path.write_text(json.dumps(manifest_data), encoding="utf-8", newline="\n")
        code = self._run_main(manifest_path, npm_lock, py_lock)
        self.assertEqual(code, 1)

    def test_declared_but_missing_lockfile_is_rejected(self):
        manifest_path, _npm_lock, py_lock = self._write_repo(
            "interface/package-lock.json", "tools/requirements-lock.txt")
        (self.tmp_path / "interface" / "package-lock.json").unlink()
        code = self._run_main(manifest_path, _npm_lock, py_lock)
        self.assertEqual(code, 1)

    def test_resolve_declared_lockfiles_rejects_escaping_declaration(self):
        manifest_path, _npm_lock, _py_lock = self._write_repo(
            "interface/package-lock.json", "tools/requirements-lock.txt")
        manifest_data = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest_data["lockfiles"]["npm"] = "../outside/package-lock.json"
        with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
            generate_sbom.resolve_declared_lockfiles(manifest_data, manifest_path)
        self.assertIn("escapes the repository root", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
