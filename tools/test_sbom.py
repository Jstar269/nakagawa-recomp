# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

from contextlib import redirect_stderr, redirect_stdout
import importlib.metadata
import io
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import generate_sbom
import nk_doctor_checks
import record_toolchain
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
        # License resolution reads installed dist metadata for the exact pins
        # (generate_sbom._resolve_python_package_license).  CI installs
        # tools/requirements-lock.txt into the test interpreter; a different
        # interpreter (e.g. a mingw/MSYS2 python selected via PATH) has its own
        # site-packages and cannot resolve these licenses at all.
        missing = []
        for pkg in packages:
            try:
                importlib.metadata.metadata(pkg["name"])
            except importlib.metadata.PackageNotFoundError:
                missing.append(pkg["name"])
        if missing:
            self.skipTest(
                "license resolution requires the locked packages installed in the "
                f"running interpreter ({sys.executable}); missing: {', '.join(missing)}"
            )
        self.assertEqual(packages[0]["license"], "GPL-3.0-or-later")
        self.assertEqual(packages[1]["license"], "MIT")
        for pkg in packages:
            self.assertNotIn(pkg["license"], {"NOASSERTION", "UNKNOWN", "unspecified"})

    def test_shipped_dlls_appear_in_sboms_with_licenses(self):
        manifest_data = {
            "name": "nakagawa-recomp",
            "version": "0.1.0",
            "license": "GPL-3.0-or-later",
            "description": "Test App",
            "components": [],
        }
        py_pkgs = []
        with tempfile.TemporaryDirectory() as tmpdir:
            pkg_dir = Path(tmpdir)
            (pkg_dir / "SDL3.dll").write_bytes(b"dummy-sdl3")
            notices_dir = pkg_dir / "THIRD_PARTY_NOTICES"
            notices_dir.mkdir()
            (notices_dir / "index.json").write_text(json.dumps({
                "schema_version": 1,
                "components": [
                    {
                        "name": "SDL3",
                        "binary": "SDL3.dll",
                        "version": "3.4.12",
                        "spdx_id": "Zlib",
                        "source_path": "share/licenses/SDL3/LICENSE.txt",
                    }
                ]
            }), encoding="utf-8")

            shipped = generate_sbom.resolve_shipped_dlls(package_dir=pkg_dir, manifest_data=manifest_data)
            self.assertEqual(len(shipped), 1)
            self.assertEqual(shipped[0]["name"], "SDL3.dll")
            self.assertEqual(shipped[0]["license"], "Zlib")

            spdx23 = generate_sbom.generate_spdx23(manifest_data, py_pkgs, shipped_dlls=shipped)
            spdx_names = {p["name"]: p.get("licenseConcluded") for p in spdx23["packages"]}
            self.assertIn("SDL3.dll", spdx_names)
            self.assertEqual(spdx_names["SDL3.dll"], "Zlib")

            spdx301 = generate_sbom.generate_spdx301(manifest_data, py_pkgs, shipped_dlls=shipped)
            graph_names = {node.get("spdx:name"): node.get("spdx:concludedLicense") for node in spdx301["@graph"]}
            self.assertIn("SDL3.dll", graph_names)
            self.assertEqual(graph_names["SDL3.dll"], "http://spdx.org/licenses/Zlib")

            cyclonedx = generate_sbom.generate_cyclonedx(manifest_data, py_pkgs, shipped_dlls=shipped)
            cdx_names = {c["name"]: c["licenses"][0]["license"]["id"] for c in cyclonedx["components"] if "licenses" in c}
            self.assertIn("SDL3.dll", cdx_names)
            self.assertEqual(cdx_names["SDL3.dll"], "Zlib")

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
        py_pkgs = generate_sbom.parse_python_lockfile(
            generate_sbom.ROOT / "tools" / "requirements-lock.txt")[:1]

        spdx = generate_sbom.generate_spdx23(manifest_data, py_pkgs)
        self.assertEqual(spdx["spdxVersion"], "SPDX-2.3")
        self.assertEqual(spdx["name"], "nakagawa-recomp-sbom")
        pkg_names = {p["name"] for p in spdx["packages"]}
        self.assertIn("nakagawa-recomp", pkg_names)
        self.assertIn("SDL3 Runtime", pkg_names)
        self.assertIn("compiledb", pkg_names)

        cyclonedx = generate_sbom.generate_cyclonedx(manifest_data, py_pkgs)
        self.assertEqual(cyclonedx["bomFormat"], "CycloneDX")
        self.assertEqual(cyclonedx["specVersion"], "1.5")
        self.assertGreater(len(cyclonedx["components"]), 1)

    def test_verify_release_locks_and_sbom(self):
        manifest_path = generate_sbom.ROOT / "assets" / "release_manifest.json"
        py_lock_path = generate_sbom.ROOT / "tools" / "requirements-lock.txt"

        errors = verify_sbom.verify_release_locks(manifest_path)
        self.assertEqual(errors, [], f"Release locks verification failed: {errors}")

        manifest_data = json.loads(manifest_path.read_text(encoding="utf-8"))
        # Build from one snapshot with standards-conformant lock evidence.
        parsed = generate_sbom.parse_lockfiles(py_lock_path, repo_root=generate_sbom.ROOT)
        py_pkgs = parsed["py_packages"]
        spdx = generate_sbom.generate_spdx23(
            manifest_data, py_pkgs,
            lock_files=parsed["lock_files"],
            lock_relationships=parsed["lock_relationships"],
        )

        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False, encoding="utf-8") as tmp:
            json.dump(spdx, tmp)
            spdx_path = Path(tmp.name)
        try:
            match_errors, _digests = verify_sbom.verify_sbom_matches(spdx_path, manifest_path, py_lock_path)
            self.assertEqual(match_errors, [], f"SPDX verification failed: {match_errors}")
        finally:
            spdx_path.unlink(missing_ok=True)

    def test_manifest_declares_only_the_native_python_tooling(self):
        manifest = json.loads((generate_sbom.ROOT / "assets" / "release_manifest.json").read_text(
            encoding="utf-8"))
        self.assertEqual(set(manifest["lockfiles"]), {"python"})
        self.assertFalse((generate_sbom.ROOT / "interface").exists())

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

        py_pkgs = generate_sbom.parse_python_lockfile(generate_sbom.ROOT / "tools" / "requirements-lock.txt")
        sbom = generate_sbom.generate_spdx23(data, py_pkgs)
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
                generate_sbom.ROOT / "tools" / "requirements-lock.txt",
            )
            self.assertTrue(any("VFPU lookup tables missing" in error for error in errors))
        finally:
            sbom_path.unlink(missing_ok=True)


class TestPythonArtifactHashVerification(unittest.TestCase):
    METADATA_PATH = generate_sbom.ROOT / "assets" / "pypi_tool_metadata_2026-09-25.json"
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
        self.assertEqual(metadata["retrieved_utc"], "2026-09-25")
        self.assertEqual(
            [source["url"] for source in metadata["sources"]],
            [
                "https://pypi.org/pypi/compiledb/0.10.7/json",
                "https://pypi.org/pypi/ruff/0.16.8/json",
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
        text = f"ruff==0.16.8 --hash=sha256:{self.OLD_RUFF_HASH}\n"
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
        py_lock = generate_sbom.ROOT / "tools" / "requirements-lock.txt"
        manifest_path = generate_sbom.ROOT / "assets" / "release_manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        parsed = generate_sbom.parse_lockfiles(py_lock, repo_root=generate_sbom.ROOT)
        spdx23 = generate_sbom.generate_spdx23(
            manifest,
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

        spdx3 = generate_sbom.generate_spdx301(manifest, parsed["py_packages"])
        spdx3_package = next(
            package for package in spdx3["@graph"]
            if package.get("spdx:name") == "ruff"
        )
        self.assertEqual(
            {item["spdx:hashValue"] for item in spdx3_package["spdx:verifiedUsing"]},
            verified,
        )
        self.assertIn("status=declared", spdx3_package["spdx:sourceInfo"])

        cyclonedx = generate_sbom.generate_cyclonedx(manifest, parsed["py_packages"])
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
        py_lock = generate_sbom.ROOT / "tools" / "requirements-lock.txt"
        manifest_path = generate_sbom.ROOT / "assets" / "release_manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        parsed = generate_sbom.parse_lockfiles(py_lock, repo_root=generate_sbom.ROOT)
        spdx = generate_sbom.generate_spdx23(
            manifest,
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
                spdx_path, manifest_path, py_lock)
        finally:
            spdx_path.unlink(missing_ok=True)
        self.assertTrue(
            any("verified Python artifact hash metadata mismatch" in error for error in errors),
            str(errors),
        )


class TestToolchainPolicyVerification(unittest.TestCase):
    MANIFEST_PATH = generate_sbom.ROOT / "assets" / "release_manifest.json"

    def _manifest_with_policy(self, policy: object) -> Path:
        data = json.loads(self.MANIFEST_PATH.read_text(encoding="utf-8"))
        if policy is None:
            data.pop("toolchain_policy", None)
        else:
            data["toolchain_policy"] = policy
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False, encoding="utf-8") as tmp:
            json.dump(data, tmp)
            return Path(tmp.name)

    def test_bogus_declared_component_fails(self):
        # 1. Unknown component fails
        bad_unknown = {
            "compiler": ">=4.9.0 (rolling-msys2)",
            "make": ">=3.81 (rolling-msys2)",
            "python": ">=3.14,<3.15",
            "sdl3": ">=3.0.0 (rolling-msys2)",
            "vulkan_sdk": ">=1.1.0",
            "clang": ">=15.0.0",
        }
        path = self._manifest_with_policy(bad_unknown)
        try:
            errors = verify_sbom.verify_release_locks(path)
            self.assertTrue(any("unknown component in toolchain_policy: clang" in e for e in errors), str(errors))
        finally:
            path.unlink(missing_ok=True)

        # 2. Missing required component fails
        bad_missing = {
            "make": ">=3.81 (rolling-msys2)",
            "python": ">=3.14,<3.15",
            "sdl3": ">=3.0.0 (rolling-msys2)",
            "vulkan_sdk": ">=1.1.0",
        }
        path = self._manifest_with_policy(bad_missing)
        try:
            errors = verify_sbom.verify_release_locks(path)
            self.assertTrue(any("missing required component in toolchain_policy: compiler" in e for e in errors), str(errors))
        finally:
            path.unlink(missing_ok=True)

        # 3. Invalid range syntax fails
        bad_syntax = {
            "compiler": "invalid-syntax",
            "make": ">=3.81 (rolling-msys2)",
            "python": ">=3.14,<3.15",
            "sdl3": ">=3.0.0 (rolling-msys2)",
            "vulkan_sdk": ">=1.1.0",
        }
        path = self._manifest_with_policy(bad_syntax)
        try:
            errors = verify_sbom.verify_release_locks(path)
            self.assertTrue(any("invalid range syntax" in e for e in errors), str(errors))
        finally:
            path.unlink(missing_ok=True)

        # 4. Missing toolchain_policy object fails
        path = self._manifest_with_policy(None)
        try:
            errors = verify_sbom.verify_release_locks(path)
            self.assertTrue(any("missing toolchain_policy object" in e for e in errors), str(errors))
        finally:
            path.unlink(missing_ok=True)

    def test_observed_version_outside_policy_fails_mutations(self):
        valid_observed = {
            "compiler": "16.1.0",
            "make": "4.4.1",
            "python": "3.14.7",
            "sdl3": "3.4.12",
            "vulkan_sdk": "1.4.357.0",
        }

        # Mutation 1: Compiler outside policy (< 13.0.0)
        mutated_compiler = dict(valid_observed, compiler="4.8.5")
        errors = verify_sbom.verify_release_locks(self.MANIFEST_PATH, observed_toolchain=mutated_compiler)
        self.assertTrue(
            any("observed toolchain component 'compiler' version '4.8.5' does not satisfy policy" in e for e in errors),
            str(errors),
        )

        # Mutation 2: SDL3 outside policy (< 3.0.0)
        mutated_sdl = dict(valid_observed, sdl3="2.28.5")
        errors = verify_sbom.verify_release_locks(self.MANIFEST_PATH, observed_toolchain=mutated_sdl)
        self.assertTrue(
            any("observed toolchain component 'sdl3' version '2.28.5' does not satisfy policy" in e for e in errors),
            str(errors),
        )

        # Mutation 3: Vulkan SDK outside policy (< 1.3.0)
        mutated_vk = dict(valid_observed, vulkan_sdk="1.0.65.0")
        errors = verify_sbom.verify_release_locks(self.MANIFEST_PATH, observed_toolchain=mutated_vk)
        self.assertTrue(
            any("observed toolchain component 'vulkan_sdk' version '1.0.65.0' does not satisfy policy" in e for e in errors),
            str(errors),
        )

        # Mutation 4: Python outside policy (< 3.14 or >= 3.15)
        mutated_py_old = dict(valid_observed, python="3.13.9")
        errors = verify_sbom.verify_release_locks(self.MANIFEST_PATH, observed_toolchain=mutated_py_old)
        self.assertTrue(
            any("observed toolchain component 'python' version '3.13.9' does not satisfy policy" in e for e in errors),
            str(errors),
        )

        mutated_py_new = dict(valid_observed, python="3.15.0")
        errors = verify_sbom.verify_release_locks(self.MANIFEST_PATH, observed_toolchain=mutated_py_new)
        self.assertTrue(
            any("observed toolchain component 'python' version '3.15.0' does not satisfy policy" in e for e in errors),
            str(errors),
        )

    def test_in_policy_observation_passes(self):
        valid_observed = {
            "compiler": "16.1.0",
            "make": "4.4.1",
            "python": "3.14.7",
            "sdl3": "3.4.12",
            "vulkan_sdk": "1.4.357.0",
        }
        errors = verify_sbom.verify_release_locks(self.MANIFEST_PATH, observed_toolchain=valid_observed)
        self.assertEqual(errors, [])

        # Also passes when supplied as structured objects with 'version' key
        structured_observed = {
            k: {"version": v, "path": f"/mock/path/{k}"}
            for k, v in valid_observed.items()
        }
        errors = verify_sbom.verify_release_locks(self.MANIFEST_PATH, observed_toolchain=structured_observed)
        self.assertEqual(errors, [])

    def test_observed_toolchain_missing_component_fails(self):
        incomplete = {
            "make": "4.4.1",
            "python": "3.14.7",
            "sdl3": "3.4.12",
            "vulkan_sdk": "1.4.357.0",
        }
        errors = verify_sbom.verify_release_locks(self.MANIFEST_PATH, observed_toolchain=incomplete)
        self.assertTrue(
            any("observed toolchain missing required component: compiler" in e for e in errors),
            str(errors),
        )

    def test_success_message_makes_no_reproducibility_claim(self):
        out_buf = io.StringIO()
        err_buf = io.StringIO()
        with redirect_stdout(out_buf), redirect_stderr(err_buf):
            code = verify_sbom.main(["--manifest", str(self.MANIFEST_PATH)])
        self.assertEqual(code, 0)
        output = out_buf.getvalue()
        self.assertIn("SBOM Verification: OK (Release dependency lockfile, artifact hashes, toolchain policy verified)", output)
        self.assertNotIn("reproducible", output.lower())
        self.assertNotIn("all release dependency locks", output.lower())

        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False, encoding="utf-8") as tmp:
            json.dump({
                "compiler": "16.1.0",
                "make": "4.4.1",
                "python": "3.14.7",
                "sdl3": "3.4.12",
                "vulkan_sdk": "1.4.357.0",
            }, tmp)
            obs_path = Path(tmp.name)
        try:
            out_buf = io.StringIO()
            err_buf = io.StringIO()
            with redirect_stdout(out_buf), redirect_stderr(err_buf):
                code = verify_sbom.main(["--manifest", str(self.MANIFEST_PATH), "--observed-toolchain", str(obs_path)])
            self.assertEqual(code, 0)
            output = out_buf.getvalue()
            self.assertIn(
                "SBOM Verification: OK (Release dependency lockfile, artifact hashes, toolchain policy, observed toolchain verified)",
                output,
            )
            self.assertNotIn("reproducible", output.lower())
        finally:
            obs_path.unlink(missing_ok=True)

    def test_live_toolchain_recorder_and_verification(self):
        # Inspects the real build environment; hosts without the native toolchain (for example
        # the Linux Python-only CI shards, which have no SDL3) skip rather than fail.
        try:
            recorded = record_toolchain.record_observed_toolchain()
        except (nk_doctor_checks.Sdl3ProviderError, FileNotFoundError, OSError) as exc:
            self.skipTest(f"native toolchain not available on this host: {exc}")
        expected = {"compiler", "make", "python", "sdl3", "vulkan_sdk"}
        self.assertEqual(set(recorded.keys()), expected)
        for comp in expected:
            self.assertIn("version", recorded[comp])
            self.assertTrue(recorded[comp]["version"])
        errors = verify_sbom.verify_release_locks(self.MANIFEST_PATH, observed_toolchain=recorded)
        self.assertEqual(errors, [])


if __name__ == "__main__":
    unittest.main()
