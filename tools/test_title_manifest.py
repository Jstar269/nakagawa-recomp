# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import title_manifest


class TitleManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.fixture_path = ROOT / "assets" / "titles" / "synthetic.json"
        self.fixture = title_manifest.load_manifest(self.fixture_path)

    def test_checked_in_fixture_is_valid_and_canonicalization_is_stable(self) -> None:
        normalized = title_manifest.validate_manifest(self.fixture)
        first = title_manifest.canonical_json(normalized)
        second = title_manifest.canonical_json(json.loads(first))
        self.assertEqual(first, second)
        self.assertEqual(normalized["id"], "synthetic-allegrex-v1")
        self.assertEqual(normalized["codegen_profile"], "none")
        self.assertEqual(normalized["feature_requirements"], sorted(normalized["feature_requirements"]))
        self.assertNotIn("disc", normalized)

    def test_schema_is_parseable_strict_and_matches_root_contract(self) -> None:
        schema = json.loads((ROOT / "assets" / "title_manifest.schema.json").read_text(encoding="utf-8"))
        self.assertEqual(schema["$schema"], "https://json-schema.org/draft/2020-12/schema")
        self.assertFalse(schema["additionalProperties"])
        self.assertEqual(schema["properties"]["schema_version"]["const"], 1)
        self.assertEqual(schema["properties"]["codegen_profile"]["enum"], ["none", "hst"])
        self.assertEqual(
            set(schema["required"]),
            {
                "schema_version", "id", "display_name", "kind", "executable",
                "modules", "filesystem", "hle_profile", "feature_requirements",
                "verification_profile",
            },
        )

    def test_duplicate_json_key_is_rejected(self) -> None:
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "duplicate JSON object key"):
            title_manifest.loads_manifest('{"schema_version":1,"schema_version":1}')

    def test_unknown_root_and_nested_fields_are_rejected(self) -> None:
        root_bad = copy.deepcopy(self.fixture)
        root_bad["workspace_binding"] = "private/input.bin"
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "unknown field"):
            title_manifest.validate_manifest(root_bad)

        nested_bad = copy.deepcopy(self.fixture)
        nested_bad["executable"]["retail_hash"] = "00" * 32
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "unknown field"):
            title_manifest.validate_manifest(nested_bad)

    def test_retail_disc_policy_is_explicit(self) -> None:
        retail = copy.deepcopy(self.fixture)
        retail["kind"] = "retail"
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "require disc"):
            title_manifest.validate_manifest(retail)

        retail["disc"] = {
            "id": "TEST00001",
            "region": "NA",
            "revision_policy": "exact-disc-id",
        }
        normalized = title_manifest.validate_manifest(retail)
        self.assertEqual(normalized["disc"]["id"], "TEST00001")

        retail["disc"]["compatible_revisions"] = ["TEST00002"]
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "forbidden"):
            title_manifest.validate_manifest(retail)

        retail["disc"] = {
            "id": "TEST00001",
            "region": "NA",
            "revision_policy": "explicit-compatible-revisions",
        }
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "compatible_revisions is required"):
            title_manifest.validate_manifest(retail)

        retail["disc"]["compatible_revisions"] = ["TEST00003", "TEST00002"]
        normalized = title_manifest.validate_manifest(retail)
        self.assertEqual(normalized["disc"]["compatible_revisions"], ["TEST00002", "TEST00003"])

    def test_nonretail_manifest_cannot_smuggle_disc_metadata(self) -> None:
        value = copy.deepcopy(self.fixture)
        value["disc"] = {"id": "TEST00001", "region": "NA", "revision_policy": "exact-disc-id"}
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "only for retail"):
            title_manifest.validate_manifest(value)

    def test_uint32_boundaries_and_bool_rejection(self) -> None:
        value = copy.deepcopy(self.fixture)
        value["executable"]["base"] = title_manifest.UINT32_MAX
        self.assertEqual(title_manifest.validate_manifest(value)["executable"]["base"], title_manifest.UINT32_MAX)

        value["executable"]["base"] = title_manifest.UINT32_MAX + 1
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "range"):
            title_manifest.validate_manifest(value)

        value["executable"]["base"] = True
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "integer"):
            title_manifest.validate_manifest(value)

    def test_extra_spans_are_sorted_and_must_not_overlap(self) -> None:
        value = copy.deepcopy(self.fixture)
        value["executable"]["extra_executable_spans"] = [
            {"start": 0x3000, "end": 0x4000},
            {"start": 0x1000, "end": 0x2000},
        ]
        normalized = title_manifest.validate_manifest(value)
        self.assertEqual(normalized["executable"]["extra_executable_spans"][0]["start"], 0x1000)

        value["executable"]["extra_executable_spans"] = [
            {"start": 0x1000, "end": 0x3000},
            {"start": 0x2000, "end": 0x4000},
        ]
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "must not overlap"):
            title_manifest.validate_manifest(value)

        value["executable"]["extra_executable_spans"] = [{"start": 1, "end": 1}]
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "greater than start"):
            title_manifest.validate_manifest(value)

    def test_module_identity_and_role_invariants(self) -> None:
        value = copy.deepcopy(self.fixture)
        duplicate = copy.deepcopy(value["modules"][0])
        duplicate["name"] = "SYNTHETIC.PRX"
        duplicate["load_address"] += 0x1000
        value["modules"].append(duplicate)
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "duplicate module name"):
            title_manifest.validate_manifest(value)

        value = copy.deepcopy(self.fixture)
        duplicate = copy.deepcopy(value["modules"][0])
        duplicate["name"] = "other.prx"
        value["modules"].append(duplicate)
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "duplicate module load address"):
            title_manifest.validate_manifest(value)

        value = copy.deepcopy(self.fixture)
        value["modules"][0]["required"] = True
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "cannot be marked required"):
            title_manifest.validate_manifest(value)

        value = copy.deepcopy(self.fixture)
        value["modules"][0]["name"] = "CON.prx"
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "reserved Windows filename"):
            title_manifest.validate_manifest(value)

    def test_private_or_nonportable_paths_are_rejected(self) -> None:
        # Assemble the hostile Windows example at runtime so the publication
        # audit does not mistake this negative fixture for a real local path.
        windows_user_path = "C:" + "/" + "Users" + "/" + "example" + "/" + "game"
        bad_paths = [
            "../private",
            "/absolute/path",
            windows_user_path,
            "folder\\child",
            "folder//child",
            "folder/./child",
            "CON/file",
        ]
        for bad_path in bad_paths:
            with self.subTest(path=bad_path):
                value = copy.deepcopy(self.fixture)
                value["filesystem"]["data_root"] = bad_path
                with self.assertRaises(title_manifest.TitleManifestError):
                    title_manifest.validate_manifest(value)

    def test_duplicate_features_and_device_prefixes_are_rejected(self) -> None:
        value = copy.deepcopy(self.fixture)
        value["feature_requirements"].append(value["feature_requirements"][0])
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "duplicate feature"):
            title_manifest.validate_manifest(value)

        value = copy.deepcopy(self.fixture)
        value["filesystem"]["device_prefixes"] = ["host0:", "HOST0:"]
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "duplicate device prefix"):
            title_manifest.validate_manifest(value)

    def test_input_size_limit_and_symlink_policy(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            oversized = root / "oversized.json"
            oversized.write_text("{}" + (" " * 32), encoding="utf-8")
            with self.assertRaisesRegex(title_manifest.TitleManifestError, "exceeds"):
                title_manifest.load_manifest(oversized, max_bytes=16)

            link = root / "link.json"
            try:
                link.symlink_to(self.fixture_path)
            except OSError:
                self.skipTest("symlink creation unavailable on this host")
            with self.assertRaisesRegex(title_manifest.TitleManifestError, "symbolic links"):
                title_manifest.load_manifest(link)

    def test_normalized_write_is_exact_and_reparseable(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "normalized.json"
            title_manifest.write_normalized(output, self.fixture)
            rendered = output.read_text(encoding="utf-8")
            self.assertTrue(rendered.endswith("\n"))
            self.assertEqual(rendered, title_manifest.canonical_json(self.fixture))
            reparsed = title_manifest.validate_manifest(title_manifest.load_manifest(output))
            self.assertEqual(reparsed["id"], self.fixture["id"])

    def test_cli_success_and_failure(self) -> None:
        self.assertEqual(title_manifest.main([str(self.fixture_path)]), 0)
        with tempfile.TemporaryDirectory() as temp_dir:
            bad = Path(temp_dir) / "bad.json"
            bad.write_text('{"schema_version":2}', encoding="utf-8")
            self.assertEqual(title_manifest.main([str(bad)]), 2)

    def test_codegen_profile_is_bounded_when_present(self) -> None:
        value = copy.deepcopy(self.fixture)
        value["codegen_profile"] = "unknown"
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "unsupported codegen profile"):
            title_manifest.validate_manifest(value)

    def test_all_checked_in_manifests_validate_and_normalize_stably(self) -> None:
        titles = sorted((ROOT / "assets" / "titles").glob("*.json"))
        self.assertGreaterEqual(len(titles), 3)
        for path in titles:
            with self.subTest(manifest=path.name):
                normalized = title_manifest.validate_manifest(
                    title_manifest.load_manifest(path)
                )
                first = title_manifest.canonical_json(normalized)
                second = title_manifest.canonical_json(json.loads(first))
                self.assertEqual(first, second)
                self.assertEqual(normalized["schema_version"], 1)


if __name__ == "__main__":
    unittest.main()
