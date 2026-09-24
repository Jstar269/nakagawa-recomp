# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

import json
from pathlib import Path
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import generate_sbom
import verify_sbom


class TestSBOMTooling(unittest.TestCase):
    def test_parse_python_lockfile(self):
        packages = generate_sbom.parse_python_lockfile(
            generate_sbom.ROOT / "tools" / "requirements-lock.txt")
        self.assertEqual(len(packages), 4)
        self.assertEqual(packages[0]["name"], "compiledb")
        self.assertEqual(packages[0]["version"], "0.10.7")
        self.assertEqual(
            packages[0]["sha256"],
            "4c07cbb37b105951218e52e00fc4a8211fafc5f7eb7821d0a33c4225d7b28ecf",
        )
        self.assertEqual(len(packages[0]["declared_sha256"]), 2)
        self.assertEqual(len(packages[1]["declared_sha256"]), 18)

    def test_ruff_lock_matches_pre_commit_pin(self):
        lock_text = (generate_sbom.ROOT / "tools" / "requirements-lock.txt").read_text(
            encoding="utf-8")
        lock_match = re.search(r"(?m)^ruff==([0-9A-Za-z_.-]+)", lock_text)
        self.assertIsNotNone(lock_match)
        pre_commit_text = (generate_sbom.ROOT / ".pre-commit-config.yaml").read_text(
            encoding="utf-8")
        pre_commit_match = re.search(
            r"(?m)^  - repo: https://github[.]com/astral-sh/ruff-pre-commit\n"
            r"    rev: [^ ]+ # v([0-9]+[.][0-9]+[.][0-9]+)$",
            pre_commit_text,
        )
        self.assertIsNotNone(pre_commit_match)
        self.assertEqual(lock_match.group(1), pre_commit_match.group(1))

    def test_generate_spdx23_and_cyclonedx(self):
        manifest_data = {
            "name": "nakagawa-recomp",
            "version": "0.1.0",
            "license": "GPL-3.0-or-later",
            "description": "Test App",
            "components": [
                {"id": "sdl3-runtime", "name": "SDL3 Runtime", "license": "Zlib"}
            ]
        }
        npm_pkgs = [
            {"name": "react", "version": "18.2.0", "spdx_id": "SPDXRef-npm-react-18.2.0", "license": "MIT", "resolved": "", "purl": "pkg:npm/react@18.2.0"}
        ]
        py_pkgs = generate_sbom.parse_python_lockfile(
            generate_sbom.ROOT / "tools" / "requirements-lock.txt")[:1]

        spdx = generate_sbom.generate_spdx23(manifest_data, npm_pkgs, py_pkgs)
        self.assertEqual(spdx["spdxVersion"], "SPDX-2.3")
        self.assertEqual(spdx["name"], "nakagawa-recomp-sbom")
        pkg_names = {p["name"] for p in spdx["packages"]}
        self.assertIn("nakagawa-recomp", pkg_names)
        self.assertIn("SDL3 Runtime", pkg_names)
        self.assertIn("react", pkg_names)
        self.assertIn("compiledb", pkg_names)

        cyclonedx = generate_sbom.generate_cyclonedx(manifest_data, npm_pkgs, py_pkgs)
        self.assertEqual(cyclonedx["bomFormat"], "CycloneDX")
        self.assertEqual(cyclonedx["specVersion"], "1.5")
        self.assertGreater(len(cyclonedx["components"]), 1)

    def test_verify_release_locks_and_sbom(self):
        manifest_path = generate_sbom.ROOT / "assets" / "release_manifest.json"
        npm_lock_path = generate_sbom.ROOT / "interface" / "package-lock.json"
        py_lock_path = generate_sbom.ROOT / "tools" / "requirements-lock.txt"

        errors = verify_sbom.verify_release_locks(manifest_path)
        self.assertEqual(errors, [], f"Release locks verification failed: {errors}")

        manifest_data = json.loads(manifest_path.read_text(encoding="utf-8"))
        # Build from one snapshot per lockfile with standards-conformant lock
        # evidence, exactly as the generator CLI does.
        parsed = generate_sbom.parse_lockfiles(npm_lock_path, py_lock_path,
                                               repo_root=generate_sbom.ROOT)
        npm_pkgs = parsed["npm_packages"]
        py_pkgs = parsed["py_packages"]
        spdx = generate_sbom.generate_spdx23(
            manifest_data, npm_pkgs, py_pkgs,
            lock_files=parsed["lock_files"],
            lock_relationships=parsed["lock_relationships"],
        )

        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False, encoding="utf-8") as tmp:
            json.dump(spdx, tmp)
            spdx_path = Path(tmp.name)
        try:
            match_errors, _digests = verify_sbom.verify_sbom_matches(spdx_path, manifest_path, npm_lock_path, py_lock_path)
            self.assertEqual(match_errors, [], f"SPDX verification failed: {match_errors}")
        finally:
            spdx_path.unlink(missing_ok=True)

    def test_verify_dashboard_toolchain_compatibility(self):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False, encoding="utf-8") as tmp:
            json.dump({"devDependencies": {"typescript": "^7.0.2", "eslint": "^10.8.0"}}, tmp)
            bad_pkg = Path(tmp.name)
        try:
            errors = verify_sbom.verify_dashboard_toolchain_compatibility(bad_pkg)
            self.assertEqual(len(errors), 2)
            self.assertIn("typescript-eslint v8 requires typescript < 6.1.0", errors[0])
            self.assertIn("eslint-config-next 16.x requires eslint < 10.0.0", errors[1])
        finally:
            bad_pkg.unlink(missing_ok=True)

    def test_provenance_family_inventory_is_independent_and_fail_closed(self):
        data = json.loads((generate_sbom.ROOT / "assets" / "release_manifest.json").read_text(encoding="utf-8"))
        errors = verify_sbom.verify_provenance_families(data)
        self.assertEqual(errors, [])

        mutated = json.loads(json.dumps(data))
        mutated["provenance_families"] = [
            family for family in mutated["provenance_families"] if family["id"] != "vfpu"
        ]
        self.assertTrue(
            any("provenance family missing: vfpu" in error
                for error in verify_sbom.verify_provenance_families(mutated))
        )

        npm_pkgs = generate_sbom.parse_npm_lockfile(generate_sbom.ROOT / "interface" / "package-lock.json")
        py_pkgs = generate_sbom.parse_python_lockfile(generate_sbom.ROOT / "tools" / "requirements-lock.txt")
        sbom = generate_sbom.generate_spdx23(data, npm_pkgs, py_pkgs)
        sbom["packages"] = [
            package for package in sbom["packages"]
            if package.get("name") != "PPSSPP-origin VFPU lookup tables"
        ]
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False, encoding="utf-8") as tmp:
            json.dump(sbom, tmp)
            sbom_path = Path(tmp.name)
        try:
            errors, _digests = verify_sbom.verify_sbom_matches(
                sbom_path,
                generate_sbom.ROOT / "assets" / "release_manifest.json",
                generate_sbom.ROOT / "interface" / "package-lock.json",
                generate_sbom.ROOT / "tools" / "requirements-lock.txt",
            )
            self.assertTrue(any("VFPU lookup tables missing" in error for error in errors))
        finally:
            sbom_path.unlink(missing_ok=True)


class TestPythonArtifactHashVerification(unittest.TestCase):
    METADATA_PATH = generate_sbom.ROOT / "assets" / "pypi_tool_metadata_2026-09-24.json"
    COMPILEDB_WHEEL_HASH = "4c07cbb37b105951218e52e00fc4a8211fafc5f7eb7821d0a33c4225d7b28ecf"
    OLD_RUFF_HASH = "7f9c8f2b3c1d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f"

    def _metadata(self) -> dict:
        return json.loads(self.METADATA_PATH.read_text(encoding="utf-8"))

    def _parse(self, text: str) -> list[dict]:
        with tempfile.NamedTemporaryFile(mode="w", suffix=".txt", delete=False, encoding="utf-8") as tmp:
            tmp.write(text)
            tmp_path = Path(tmp.name)
        try:
            return generate_sbom.parse_python_lockfile(tmp_path)
        finally:
            tmp_path.unlink(missing_ok=True)

    def test_repository_lock_matches_every_trusted_artifact(self):
        packages = generate_sbom.parse_python_lockfile(
            generate_sbom.ROOT / "tools" / "requirements-lock.txt")
        metadata = self._metadata()
        self.assertEqual(metadata["retrieved_utc"], "2026-09-24")
        self.assertEqual(
            [source["url"] for source in metadata["sources"]],
            [
                "https://pypi.org/pypi/compiledb/0.10.7/json",
                "https://pypi.org/pypi/ruff/0.16.0/json",
                "https://pypi.org/pypi/click/8.5.0/json",
                "https://pypi.org/pypi/bashlex/0.18/json",
            ],
        )
        sources = {source["name"]: source for source in metadata["sources"]}
        self.assertEqual(sum(len(source["artifacts"]) for source in sources.values()), 24)
        self.assertEqual(
            [package["name"] for package in packages],
            ["compiledb", "ruff", "click", "bashlex"],
        )
        for package in packages:
            expected = {
                artifact["sha256"]
                for artifact in sources[package["name"]]["artifacts"]
            }
            self.assertEqual(set(package["declared_sha256"]), expected)
            self.assertEqual(set(package["verified_sha256"]), expected)

    def test_declared_hash_matching_no_artifact_fails(self):
        text = (
            "compiledb==0.10.7 "
            f"--hash=sha256:{self.COMPILEDB_WHEEL_HASH} "
            f"--hash=sha256:{'f' * 64}\n"
        )
        with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
            self._parse(text)
        self.assertIn("matches no trusted artifact", str(ctx.exception))

    def test_missing_hash_fails(self):
        with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
            self._parse("compiledb==0.10.7\n")
        self.assertIn("has no SHA-256 hashes", str(ctx.exception))

    def test_malformed_hash_fails(self):
        text = f"compiledb==0.10.7 --hash=sha256:{'A' * 64}\n"
        with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
            self._parse(text)
        self.assertIn("malformed SHA-256 hash", str(ctx.exception))

    def test_old_sequential_ruff_hash_fails_as_unmatched(self):
        text = f"ruff==0.16.0 --hash=sha256:{self.OLD_RUFF_HASH}\n"
        with self.assertRaises(generate_sbom.LockfileParseError) as ctx:
            self._parse(text)
        self.assertIn("matches no trusted artifact", str(ctx.exception))

    def test_ci_installs_python_tools_from_hashed_lock(self):
        workflow = (generate_sbom.ROOT / ".github" / "workflows" / "ci.yml").read_text(
            encoding="utf-8")
        self.assertIn(
            "--require-hashes -r tools/requirements-lock.txt",
            workflow,
        )
        self.assertIn("--no-deps .", workflow)

    def test_verifier_uses_supplied_offline_metadata(self):
        metadata = self._metadata()
        metadata["sources"][0]["artifacts"][0]["sha256"] = "0" * 64
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False, encoding="utf-8") as tmp:
            json.dump(metadata, tmp)
            metadata_path = Path(tmp.name)
        try:
            errors = verify_sbom.verify_release_locks(
                generate_sbom.ROOT / "assets" / "release_manifest.json",
                release_index_path=metadata_path,
            )
        finally:
            metadata_path.unlink(missing_ok=True)
        self.assertTrue(any("matches no trusted artifact" in error for error in errors), str(errors))

    def test_sboms_distinguish_declared_and_verified_hashes(self):
        npm_lock = generate_sbom.ROOT / "interface" / "package-lock.json"
        py_lock = generate_sbom.ROOT / "tools" / "requirements-lock.txt"
        manifest_path = generate_sbom.ROOT / "assets" / "release_manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        parsed = generate_sbom.parse_lockfiles(npm_lock, py_lock, repo_root=generate_sbom.ROOT)
        spdx23 = generate_sbom.generate_spdx23(
            manifest,
            parsed["npm_packages"],
            parsed["py_packages"],
            lock_files=parsed["lock_files"],
            lock_relationships=parsed["lock_relationships"],
        )
        spdx_package = next(
            package for package in spdx23["packages"]
            if package.get("name") == "ruff"
        )
        verified = set(parsed["py_packages"][1]["verified_sha256"])
        self.assertEqual(
            {checksum["checksumValue"] for checksum in spdx_package["checksums"]},
            verified,
        )
        comments = [annotation["comment"] for annotation in spdx_package["annotations"]]
        self.assertTrue(any("status=declared" in comment for comment in comments))
        self.assertTrue(any("status=verified" in comment for comment in comments))

        spdx3 = generate_sbom.generate_spdx301(
            manifest, parsed["npm_packages"], parsed["py_packages"])
        spdx3_package = next(
            package for package in spdx3["@graph"]
            if package.get("spdx:name") == "ruff"
        )
        self.assertEqual(
            {item["spdx:hashValue"] for item in spdx3_package["spdx:verifiedUsing"]},
            verified,
        )
        self.assertIn("status=declared", spdx3_package["spdx:sourceInfo"])

        cyclonedx = generate_sbom.generate_cyclonedx(
            manifest, parsed["npm_packages"], parsed["py_packages"])
        cdx_package = next(
            component for component in cyclonedx["components"]
            if component.get("name") == "ruff"
        )
        self.assertEqual(
            {item["content"] for item in cdx_package["hashes"]},
            verified,
        )
        properties_text = json.dumps(cdx_package["properties"])
        self.assertIn("status=declared", properties_text)
        self.assertIn("status=verified", properties_text)

    def test_verifier_rejects_tampered_verified_hash_metadata(self):
        npm_lock = generate_sbom.ROOT / "interface" / "package-lock.json"
        py_lock = generate_sbom.ROOT / "tools" / "requirements-lock.txt"
        manifest_path = generate_sbom.ROOT / "assets" / "release_manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        parsed = generate_sbom.parse_lockfiles(npm_lock, py_lock, repo_root=generate_sbom.ROOT)
        spdx = generate_sbom.generate_spdx23(
            manifest,
            parsed["npm_packages"],
            parsed["py_packages"],
            lock_files=parsed["lock_files"],
            lock_relationships=parsed["lock_relationships"],
        )
        package = next(item for item in spdx["packages"] if item.get("name") == "compiledb")
        package["checksums"] = [{"algorithm": "SHA256", "checksumValue": "f" * 64}]
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False, encoding="utf-8") as tmp:
            json.dump(spdx, tmp)
            spdx_path = Path(tmp.name)
        try:
            errors, _digests = verify_sbom.verify_sbom_matches(
                spdx_path, manifest_path, npm_lock, py_lock)
        finally:
            spdx_path.unlink(missing_ok=True)
        self.assertTrue(
            any("verified Python artifact hash metadata mismatch" in error for error in errors),
            str(errors),
        )


if __name__ == "__main__":
    unittest.main()
