# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
#
# test_release_manifest.py — regression suite for the release dependency
# manifest (assets/release_manifest.json) and the SBOM generators in
# tools/generate_sbom.py.
#
# History: PR #294 (issue #149) replaced generate_sbom.py's API wholesale
# (load_release_manifest/build_spdx_sbom/build_spdx3_sbom/SPDX3_NO_ASSERTION_IRI/
# calculate_sha256/check_sbom_freshness -> parse_*_lockfile + generate_spdx23/
# generate_spdx301/generate_cyclonedx) but was merged without updating this
# test, leaving the Python gate red on main (issue #298). This file now tests
# the ACTUAL current API: the three format generators, the CLI entry point,
# and the manifest-integrity properties that are independent of the generator
# surface (component presence, git tracking, declared-vs-actual sha256).

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import generate_sbom
from build_public_export import public_safe_excluded_paths

RELEASE_MANIFEST = ROOT / "assets" / "release_manifest.json"
NPM_LOCK = ROOT / "interface" / "package-lock.json"
PY_LOCK = ROOT / "tools" / "requirements-lock.txt"


def load_manifest() -> dict:
    if not RELEASE_MANIFEST.is_file():
        raise AssertionError("assets/release_manifest.json must exist")
    with RELEASE_MANIFEST.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def load_sbom_inputs() -> tuple[dict, list[dict], list[dict]]:
    data = load_manifest()
    npm_packages = generate_sbom.parse_npm_lockfile(NPM_LOCK)
    py_packages = generate_sbom.parse_python_lockfile(PY_LOCK)
    return data, npm_packages, py_packages


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


class TestReleaseManifest(unittest.TestCase):
    def test_manifest_basics(self):
        data = load_manifest()
        self.assertEqual(data["name"], "nakagawa-recomp")
        self.assertEqual(data["license"], "GPL-3.0-or-later")

    def test_generate_spdx23_shape(self):
        data, npm_packages, py_packages = load_sbom_inputs()
        sbom = generate_sbom.generate_spdx23(data, npm_packages, py_packages)
        self.assertEqual(sbom["spdxVersion"], "SPDX-2.3")
        self.assertEqual(sbom["dataLicense"], "CC0-1.0")
        self.assertGreater(len(sbom["packages"]), 1, "SPDX 2.3 must include lockfile-derived packages")

        root_pkg = next(
            p for p in sbom["packages"] if p["SPDXID"] == "SPDXRef-Package-nakagawa-recomp"
        )
        self.assertEqual(root_pkg["licenseDeclared"], "GPL-3.0-or-later")
        self.assertEqual(root_pkg["licenseConcluded"], "GPL-3.0-or-later")

        spdx_ids = {p["SPDXID"] for p in sbom["packages"]}
        self.assertEqual(len(spdx_ids), len(sbom["packages"]), "SPDX 2.3 package IDs must be unique")

    def test_generate_spdx23_ingests_components_and_lockfiles(self):
        data, npm_packages, py_packages = load_sbom_inputs()
        sbom = generate_sbom.generate_spdx23(data, npm_packages, py_packages)

        for comp in data.get("components", []):
            c_id = f"SPDXRef-comp-{comp.get('id', 'unknown')}"
            self.assertIn(c_id, {p["SPDXID"] for p in sbom["packages"]},
                          f"component {comp.get('id')} must appear in SPDX 2.3 packages")

        npm_ids = {f"SPDXRef-npm-{pkg['name'].replace('/', '-')}-{pkg['version']}" for pkg in npm_packages}
        sbom_npm = {p["SPDXID"] for p in sbom["packages"] if p["SPDXID"].startswith("SPDXRef-npm-")}
        self.assertTrue(npm_ids.issubset(sbom_npm), "every parsed npm package must appear in SPDX 2.3")
        self.assertGreaterEqual(len(sbom_npm), len(npm_packages))

        root_id = "SPDXRef-Package-nakagawa-recomp"
        depends = [
            r for r in sbom["relationships"]
            if r["spdxElementId"] == root_id and r["relationshipType"] == "DEPENDS_ON"
        ]
        self.assertGreater(len(depends), 0, "root must DEPENDS_ON at least one npm package")

    def test_generate_spdx301_shape(self):
        data, npm_packages, py_packages = load_sbom_inputs()
        sbom3 = generate_sbom.generate_spdx301(data, npm_packages, py_packages)

        self.assertEqual(sbom3["@context"], "https://spdx.org/rdf/3.0.1/spdx-context.jsonld")
        graph = sbom3["@graph"]
        self.assertGreater(len(graph), 0)
        types = {e.get("@type") for e in graph}
        self.assertIn("spdx:SpdxDocument", types)
        self.assertIn("spdx:Package", types)

        root_pkg = next(e for e in graph if e.get("spdx:name") == "nakagawa-recomp")
        self.assertIn("spdx:concludedLicense", root_pkg)
        self.assertIn("GPL-3.0-or-later", root_pkg["spdx:concludedLicense"])

        ids = [e.get("@id") for e in graph]
        self.assertEqual(len(ids), len(set(ids)), "SPDX 3 graph element ids must be unique")

    def test_generate_cyclonedx_shape(self):
        data, npm_packages, py_packages = load_sbom_inputs()
        cd = generate_sbom.generate_cyclonedx(data, npm_packages, py_packages)

        self.assertEqual(cd["bomFormat"], "CycloneDX")
        self.assertEqual(cd["specVersion"], "1.5")
        self.assertGreater(len(cd["components"]), 1, "CycloneDX must include lockfile-derived components")

        app = cd["components"][0]
        self.assertEqual(app["type"], "application")
        self.assertEqual(app["name"], "nakagawa-recomp")
        self.assertEqual(app["licenses"][0]["license"]["id"], "GPL-3.0-or-later")

        # CycloneDX uniqueness is carried by bom-ref/purl, not the display name
        # (a package can legitimately appear at multiple install paths).
        purls = [c.get("purl") for c in cd["components"] if c.get("purl")]
        self.assertGreater(len(purls), 0, "CycloneDX libraries must carry purls")
        self.assertEqual(len(purls), len(set(purls)), "CycloneDX purls must be unique")

    def test_cli_writes_all_three_formats(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            spdx_out = tmp_path / "spdx23.json"
            spdx3_out = tmp_path / "spdx301.json"
            cd_out = tmp_path / "cyclonedx.json"

            code = generate_sbom.main([
                "--manifest", str(RELEASE_MANIFEST),
                "--npm-lock", str(NPM_LOCK),
                "--py-lock", str(PY_LOCK),
                "--spdx-out", str(spdx_out),
                "--spdx3-out", str(spdx3_out),
                "--cyclonedx-out", str(cd_out),
            ])
            self.assertEqual(code, 0)

            self.assertTrue(spdx_out.is_file())
            self.assertTrue(spdx3_out.is_file())
            self.assertTrue(cd_out.is_file())

            spdx_doc = json.loads(spdx_out.read_text(encoding="utf-8"))
            self.assertEqual(spdx_doc["spdxVersion"], "SPDX-2.3")
            spdx3_doc = json.loads(spdx3_out.read_text(encoding="utf-8"))
            self.assertIn("@graph", spdx3_doc)
            cd_doc = json.loads(cd_out.read_text(encoding="utf-8"))
            self.assertEqual(cd_doc["bomFormat"], "CycloneDX")

    def test_manifest_integrity_all_entries_and_git_tracking(self):
        data = load_manifest()
        # A materialized public-safe export keeps the full manifest but not the
        # profile-excluded component files; skip exactly those entries and hold
        # every remaining component to the same presence/tracking/hash contract.
        excluded = public_safe_excluded_paths(ROOT)

        try:
            git_ls = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True).stdout.splitlines()
            git_tracked = set(git_ls)
        except Exception:
            git_tracked = None

        for comp in data.get("components", []):
            comp_id = comp["id"]
            presence = comp.get("presence", "tracked_file")
            notice_p = comp.get("notice_path")

            if notice_p:
                notice_full = ROOT / notice_p
                self.assertTrue(notice_full.is_file(), f"Component {comp_id} notice_path {notice_p} does not exist")
                if git_tracked is not None and notice_p not in ("NOTICE.md", "LICENSE"):
                    self.assertIn(notice_p, git_tracked, f"Notice path {notice_p} must be tracked in Git")

            if presence == "tracked_file":
                src_p = comp.get("source_path")
                self.assertIsNotNone(src_p, f"Component {comp_id} missing source_path")
                if src_p in excluded:
                    continue
                full_src = ROOT / src_p
                self.assertTrue(full_src.is_file(), f"Component {comp_id} source_path {src_p} does not exist on disk")

                if git_tracked is not None:
                    self.assertIn(src_p, git_tracked, f"Component {comp_id} source_path {src_p} must be tracked in Git")

                declared_sha = comp.get("hashes", {}).get("sha256")
                self.assertIsNotNone(declared_sha, f"Component {comp_id} missing sha256 hash")
                self.assertTrue(len(declared_sha) == 64 and all(c in "0123456789abcdefABCDEF" for c in declared_sha),
                                f"Component {comp_id} sha256 is not 64-hex string: {declared_sha}")
                self.assertNotEqual(len(declared_sha), 40, f"Component {comp_id} sha256 confused with Git blob ID")

                actual_sha = sha256_of(full_src)
                self.assertEqual(declared_sha.lower(), actual_sha.lower(),
                                f"Component {comp_id} declared sha256 does not match actual file bytes")

            elif presence == "tracked_collection":
                prov_p = comp.get("provenance_path")
                self.assertIsNotNone(prov_p, f"Component {comp_id} missing provenance_path")
                self.assertTrue((ROOT / prov_p).is_file(), f"Component {comp_id} provenance_path {prov_p} does not exist")
                if git_tracked is not None:
                    self.assertIn(prov_p, git_tracked, f"Component {comp_id} provenance_path {prov_p} must be tracked in Git")
                self.assertNotIn("source_path", comp, f"Collection component {comp_id} must not claim a single fake source_path")

            elif presence == "notice_lineage":
                self.assertNotIn("source_path", comp, f"Lineage component {comp_id} must not claim a single fake source_path")

            elif presence == "optional_local_or_external":
                self.assertTrue(comp.get("optional", False), f"External runtime library {comp_id} must be marked optional")

    def test_vfpu_collection_has_no_fake_provenance_checksum(self):
        data = load_manifest()
        vfpu_comp = next(c for c in data.get("components", []) if c.get("id") == "vfpu-lut-tables")
        self.assertNotIn("source_path", vfpu_comp, "VFPU collection must not claim a single fake source_path")
        self.assertEqual(vfpu_comp.get("presence"), "tracked_collection")

        prov_p = vfpu_comp.get("provenance_path")
        self.assertIsNotNone(prov_p)
        self.assertTrue((ROOT / prov_p).is_file(), f"VFPU provenance_path {prov_p} must exist")


if __name__ == "__main__":
    unittest.main()
