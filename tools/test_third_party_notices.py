# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Release gate: no bundled binary or declared component ships without a license.

The gate answers one question for both release artifacts:

* native package — does every DLL a packaging route can copy, and every component
  in ``assets/third_party_components.json``, carry an SPDX identifier and a
  license text that is actually in the tree?
* dashboard standalone output — does every locked npm package carry a license
  expression, and does every license expression have a recorded obligation?

It builds the notices from the source of truth rather than from a second literal,
so adding a component without a notice is a failing test rather than a silent gap.
"""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import dashboard_notices
import package_notices
from package_notices import PackageRouteError

INVENTORY_IDENTITY = "assets/third_party_components.json"
NPM_LOCKFILE = "interface/package-lock.json"
PY_LOCKFILE = "tools/requirements-lock.txt"


def inventory() -> dict:
    return package_notices.load_component_inventory()


def all_component_records(data: dict) -> dict[str, dict]:
    """Return every recorded component keyed by a stable identifier."""
    records: dict[str, dict] = {}
    native = data["native"]
    for dll_name, record in native["dlls"].items():
        records[f"native.dlls.{dll_name}"] = record
    for index, record in enumerate(native.get("linked_components", [])):
        records[f"native.linked_components[{index}]"] = record
    for index, record in enumerate(native.get("host_resolved_components", [])):
        records[f"native.host_resolved_components[{index}]"] = record
    if "project_output" in native:
        records["native.project_output"] = native["project_output"]
    return records


class TestThirdPartyComponentInventory(unittest.TestCase):
    """Every declared component is complete enough to ship a notice for."""

    def setUp(self) -> None:
        self.data = inventory()
        self.records = all_component_records(self.data)

    def test_inventory_declares_components(self) -> None:
        self.assertGreaterEqual(len(self.records), 6, "the inventory must describe the shipped set")

    def test_every_component_has_a_resolved_license_identifier(self) -> None:
        for key, record in self.records.items():
            with self.subTest(component=key):
                spdx_id = record.get("spdx_id")
                self.assertTrue(spdx_id, f"{key} has no spdx_id")
                self.assertNotIn("NOASSERTION", str(spdx_id), f"{key} resolves to NOASSERTION")
                self.assertNotIn("UNKNOWN", str(spdx_id), f"{key} resolves to UNKNOWN")

    def test_every_component_license_text_is_present_and_nonempty(self) -> None:
        for key, record in self.records.items():
            with self.subTest(component=key):
                texts = record.get("license_texts")
                self.assertTrue(texts, f"{key} declares no license_texts")
                for entry in texts:
                    path = ROOT / entry["file"]
                    self.assertTrue(path.is_file(), f"{key} license text missing: {entry['file']}")
                    self.assertTrue(
                        path.read_text(encoding="utf-8", errors="replace").strip(),
                        f"{key} license text is empty: {entry['file']}",
                    )

    def test_recorded_license_texts_match_their_recorded_digest(self) -> None:
        """A license text may not be edited into the tree without re-recording it."""
        for key, record in self.records.items():
            for entry in record.get("license_texts", []):
                digest = entry.get("sha256")
                if digest is None:
                    continue
                with self.subTest(component=key, text=entry["file"]):
                    actual = hashlib.sha256((ROOT / entry["file"]).read_bytes()).hexdigest()
                    self.assertEqual(
                        actual, digest,
                        f"{entry['file']} no longer matches the digest recorded in {INVENTORY_IDENTITY}",
                    )

    def test_every_dll_a_copy_route_can_copy_has_a_record(self) -> None:
        """The obligation follows the copy list, not the conditional that guards it."""
        dll_records = {name.lower() for name in self.data["native"]["dlls"]}
        for route in self.data["native"]["copy_routes"]:
            for dll_name in route["copies"]:
                with self.subTest(route=route["id"], dll=dll_name):
                    self.assertIn(
                        dll_name.lower(), dll_records,
                        f"copy route {route['id']} copies {dll_name} but no license record exists for it",
                    )
                    record = self.data["native"]["dlls"][dll_name.lower()]
                    self.assertEqual(record["disposition"], "copied_into_package")

    def test_host_resolved_dlls_are_not_also_copied_into_a_package(self) -> None:
        """A host-resolved loader is recorded for attribution, never staged for redistribution."""
        staged = package_notices.packaged_dll_names()
        for record in self.data["native"].get("host_resolved_components", []):
            dll = record.get("dll", "").lower()
            if dll:
                with self.subTest(dll=dll):
                    self.assertNotIn(dll, staged)
        copied = {name.lower() for name in self.data["native"]["dlls"]}
        for host_dll in (d.lower() for d in self.data["native"].get("host_resolved_dlls", [])):
            if host_dll in copied:
                self.fail(f"{host_dll} is declared both host-resolved and copied into a package")

    def test_system_dll_patterns_cover_the_measured_player_imports(self) -> None:
        """Every DLL the measured player imports is either a system library or recorded."""
        recorded = package_notices.known_runtime_dlls()
        for dll_name in (
            "KERNEL32.dll", "SHELL32.dll", "api-ms-win-crt-runtime-l1-1-0.dll",
            "SDL3.dll", "vulkan-1.dll", "libwinpthread-1.dll", "libgcc_s_seh-1.dll",
            "libstdc++-6.dll",
        ):
            with self.subTest(dll=dll_name):
                self.assertTrue(
                    package_notices.is_system_dll(dll_name) or dll_name.lower() in recorded,
                    f"{dll_name} is neither a recognized system library nor a recorded component",
                )


class TestNativePackageNoticesGate(unittest.TestCase):
    """The native package gate fails closed for an unrecorded or textless DLL."""

    def setUp(self) -> None:
        self.tmpdir = tempfile.TemporaryDirectory()
        self.pkg_dir = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_every_dll_the_copy_route_ships_generates_a_notice(self) -> None:
        """Each DLL from the inventory's copy list gets a notice, version and text."""
        for dll_name in sorted(package_notices.packaged_dll_names()):
            with self.subTest(dll=dll_name):
                (self.pkg_dir / dll_name).write_bytes(b"MZ\0\0synthetic")
                result = package_notices.generate_package_notices(self.pkg_dir, repo_root=ROOT)
                entry = next(
                    c for c in result["components"] if c.get("binary", "").lower() == dll_name
                )
                self.assertNotIn("NOASSERTION", entry["spdx_id"])
                notices = self.pkg_dir / "THIRD_PARTY_NOTICES"
                self.assertTrue((notices / entry["license_file"]).is_file())
                for extra in entry.get("additional_license_files", []):
                    self.assertTrue((notices / extra).is_file())
                (self.pkg_dir / dll_name).unlink()

    def test_host_resolved_loader_present_in_a_package_gets_a_notice(self) -> None:
        """A user-supplied Vulkan loader is named, not waved through as a system library."""
        (self.pkg_dir / "vulkan-1.dll").write_bytes(b"MZ\0\0synthetic")
        result = package_notices.generate_package_notices(self.pkg_dir, repo_root=ROOT)
        entry = next(
            c for c in result["components"] if c.get("binary", "").lower() == "vulkan-1.dll"
        )
        self.assertEqual(entry["spdx_id"], "Apache-2.0")
        self.assertEqual(entry["disposition"], "host_resolved_not_redistributed")
        self.assertTrue((self.pkg_dir / "THIRD_PARTY_NOTICES" / "Vulkan-loader.txt").is_file())

    def test_unrecorded_dll_still_refuses_the_package(self) -> None:
        (self.pkg_dir / "mystery.dll").write_bytes(b"MZ\0\0synthetic")
        with self.assertRaises(PackageRouteError) as ctx:
            package_notices.generate_package_notices(self.pkg_dir, repo_root=ROOT)
        self.assertEqual(ctx.exception.code, "PACKAGE_LICENSE_RECORD_MISSING")

    def test_component_without_a_license_text_refuses_the_package(self) -> None:
        """Removing an in-tree license text stops packaging instead of shipping a bare DLL."""
        with tempfile.TemporaryDirectory() as repo_dir:
            fake_repo = Path(repo_dir)
            (fake_repo / "LICENSE").write_text("GPL-3.0-or-later project text", encoding="utf-8")
            atrac3p_lic = ROOT / "src" / "rt" / "atrac3p" / "LICENSE.LGPLv2.1.txt"
            (fake_repo / "src" / "rt" / "atrac3p").mkdir(parents=True)
            with self.assertRaises(PackageRouteError) as ctx:
                package_notices.generate_package_notices(self.pkg_dir, repo_root=fake_repo)
            self.assertEqual(ctx.exception.code, "PACKAGE_LICENSE_TEXT_MISSING")
            self.assertIn("FFmpeg ATRAC3+ subset", str(ctx.exception))
            self.assertTrue(atrac3p_lic.is_file(), "the real tree must still carry the LGPL text")

    def test_missing_inventory_fails_closed(self) -> None:
        """No inventory means no license record, not a silently empty record set."""
        with tempfile.TemporaryDirectory() as empty:
            with self.assertRaises(PackageRouteError) as ctx:
                package_notices.load_component_inventory(Path(empty) / "missing.json")
        self.assertEqual(ctx.exception.code, "PACKAGE_LICENSE_RECORD_MISSING")
        self.assertIn(INVENTORY_IDENTITY, str(ctx.exception))


class TestDashboardNoticesGate(unittest.TestCase):
    """The dashboard standalone output names every locked package it redistributes."""

    def setUp(self) -> None:
        self.bundle = dashboard_notices.build_dashboard_notices(repo_root=ROOT)

    def test_bundle_covers_every_locked_package(self) -> None:
        lock = json.loads((ROOT / NPM_LOCKFILE).read_text(encoding="utf-8"))
        locked = {key for key in lock["packages"] if key}
        named = {component["source_path"] for component in self.bundle["components"]}
        self.assertEqual(named, locked)

    def test_every_package_has_a_resolved_license(self) -> None:
        for component in self.bundle["components"]:
            with self.subTest(package=component["name"]):
                self.assertTrue(component["spdx_id"])
                self.assertNotIn("NOASSERTION", component["spdx_id"])
                self.assertNotIn("UNKNOWN", component["spdx_id"])

    def test_lgpl_and_copyleft_dashboard_components_are_marked_unresolved(self) -> None:
        """The bundles that need a maintainer decision are named as unresolved, not decided."""
        unresolved = {
            component["name"]
            for component in self.bundle["components"]
            if component["relink_or_attribution_unresolved"]
        }
        self.assertIn("elkjs", unresolved, "EPL-2.0 secondary-license decision is missing")
        self.assertIn("caniuse-lite", unresolved, "CC-BY-4.0 attribution is missing")
        self.assertIn("@img/sharp-libvips-linux-x64", unresolved, "libvips LGPL relink is missing")

    def test_unresolved_obligations_are_rendered_into_the_notice(self) -> None:
        rendered = dashboard_notices.render_notices(self.bundle)
        self.assertIn("UNRESOLVED OBLIGATIONS", rendered)
        self.assertIn("LGPL-3.0-or-later", rendered)
        self.assertIn("EPL-2.0", rendered)
        self.assertIn("CC-BY-4.0", rendered)
        self.assertIn("MIT", rendered)
        self.assertIn("THIRD_PARTY_LICENSES/SHADCN_UI.txt", rendered)

    def test_undocumented_license_expression_fails_closed(self) -> None:
        """A new license in the lockfile without a recorded obligation stops the build."""
        obligations = copy.deepcopy(inventory()["dashboard"]["license_obligations"])
        self.assertIsNone(
            dashboard_notices.license_obligation("Nonexistent-1.0", obligations),
            "an unknown license must not match any recorded obligation",
        )
        with tempfile.TemporaryDirectory() as tmp:
            lock = json.loads((ROOT / NPM_LOCKFILE).read_text(encoding="utf-8"))
            first_key = next(k for k in lock["packages"] if k)
            lock["packages"][first_key]["license"] = "Nonexistent-1.0"
            lock_path = Path(tmp) / "package-lock.json"
            lock_path.write_text(json.dumps(lock), encoding="utf-8")
            with self.assertRaises(dashboard_notices.DashboardNoticeError) as ctx:
                dashboard_notices.build_dashboard_notices(repo_root=ROOT, lock_path=lock_path)
        self.assertEqual(ctx.exception.code, "DASHBOARD_NOTICES_LICENSE_UNDOCUMENTED")

    def test_package_with_no_license_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            lock = json.loads((ROOT / NPM_LOCKFILE).read_text(encoding="utf-8"))
            first_key = next(k for k in lock["packages"] if k)
            lock["packages"][first_key].pop("license", None)
            lock_path = Path(tmp) / "package-lock.json"
            lock_path.write_text(json.dumps(lock), encoding="utf-8")
            with self.assertRaises(dashboard_notices.DashboardNoticeError) as ctx:
                dashboard_notices.build_dashboard_notices(repo_root=ROOT, lock_path=lock_path)
        self.assertEqual(ctx.exception.code, "DASHBOARD_NOTICES_LICENSE_MISSING")

    def test_compound_expression_picks_up_every_class_obligation(self) -> None:
        obligations = inventory()["dashboard"]["license_obligations"]
        matched = dashboard_notices.license_obligation(
            "Apache-2.0 AND LGPL-3.0-or-later", obligations
        )
        self.assertEqual(matched["classes"], ["Apache-2.0", "LGPL-3.0-or-later"])
        self.assertEqual(len(matched["unresolved"]), 1)
        # A choice expression is treated conservatively: every alternative applies.
        choice = dashboard_notices.license_obligation("(MIT OR WTFPL)", obligations)
        self.assertEqual(choice["classes"], ["MIT", "WTFPL"])


class TestPythonToolLicenses(unittest.TestCase):
    """Every entry in the Python lockfile has a recorded license in the source of truth."""

    def setUp(self) -> None:
        self.recorded = inventory()["python_packages"]["packages"]

    def test_lockfile_identity_matches_the_recorded_lockfile(self) -> None:
        self.assertEqual(inventory()["python_packages"]["lockfile"], PY_LOCKFILE)

    def test_every_locked_python_package_has_a_recorded_license(self) -> None:
        import generate_sbom

        packages = generate_sbom.parse_python_lockfile(ROOT / PY_LOCKFILE)
        self.assertTrue(packages)
        for package in packages:
            name = generate_sbom._normalize_python_name(package["name"])
            with self.subTest(package=name):
                self.assertIn(name, self.recorded, f"{name} has no recorded license in {INVENTORY_IDENTITY}")
                self.assertNotEqual(
                    generate_sbom._resolve_python_package_license(package["name"]), "NOASSERTION"
                )

    def test_recorded_version_matches_the_pinned_lockfile_version(self) -> None:
        import generate_sbom

        for package in generate_sbom.parse_python_lockfile(ROOT / PY_LOCKFILE):
            name = generate_sbom._normalize_python_name(package["name"])
            with self.subTest(package=name):
                self.assertEqual(self.recorded[name]["version"], package["version"])

    def test_issue_421_packages_are_recorded(self) -> None:
        for name, expected in (("compiledb", "GPL-3.0-or-later"), ("ruff", "MIT")):
            with self.subTest(package=name):
                self.assertEqual(self.recorded[name]["spdx_id"], expected)
                self.assertTrue(self.recorded[name]["recorded_from"])


if __name__ == "__main__":
    unittest.main()
