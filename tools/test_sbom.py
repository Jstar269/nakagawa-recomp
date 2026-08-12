# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import generate_sbom
import verify_sbom


class TestSBOMTooling(unittest.TestCase):
    def test_parse_python_lockfile(self):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".txt", delete=False, encoding="utf-8") as tmp:
            tmp.write("compiledb==0.10.7 --hash=sha256:d1d36d4df6c5c723ae4e447b973b306b3a0df47a50dfa7243c2c19e5db4d12bb\n")
            tmp.write("ruff==0.9.9\n")
            tmp_path = Path(tmp.name)
        try:
            packages = generate_sbom.parse_python_lockfile(tmp_path)
            self.assertEqual(len(packages), 2)
            self.assertEqual(packages[0]["name"], "compiledb")
            self.assertEqual(packages[0]["version"], "0.10.7")
            self.assertEqual(packages[0]["sha256"], "d1d36d4df6c5c723ae4e447b973b306b3a0df47a50dfa7243c2c19e5db4d12bb")
            self.assertEqual(packages[1]["name"], "ruff")
        finally:
            tmp_path.unlink(missing_ok=True)

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
        py_pkgs = [
            {"name": "compiledb", "version": "0.10.7", "spdx_id": "SPDXRef-pip-compiledb-0.10.7", "license": "NOASSERTION", "purl": "pkg:pypi/compiledb@0.10.7"}
        ]

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
        npm_pkgs = generate_sbom.parse_npm_lockfile(npm_lock_path)
        py_pkgs = generate_sbom.parse_python_lockfile(py_lock_path)
        spdx = generate_sbom.generate_spdx23(manifest_data, npm_pkgs, py_pkgs)

        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False, encoding="utf-8") as tmp:
            json.dump(spdx, tmp)
            spdx_path = Path(tmp.name)
        try:
            match_errors = verify_sbom.verify_sbom_matches(spdx_path, manifest_path, npm_lock_path, py_lock_path)
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
            errors = verify_sbom.verify_sbom_matches(
                sbom_path,
                generate_sbom.ROOT / "assets" / "release_manifest.json",
                generate_sbom.ROOT / "interface" / "package-lock.json",
                generate_sbom.ROOT / "tools" / "requirements-lock.txt",
            )
            self.assertTrue(any("VFPU lookup tables missing" in error for error in errors))
        finally:
            sbom_path.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
