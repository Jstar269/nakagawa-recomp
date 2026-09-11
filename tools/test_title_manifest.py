# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
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
        self.assertEqual(len(schema["allOf"]), 2)
        self.assertEqual(
            schema["$defs"]["profileZero"]["properties"]["source_program"]["properties"]["entry_symbol"]["pattern"],
            "^[A-Za-z_][A-Za-z0-9_]*$",
        )

    def test_schema_enums_equal_the_python_vocabularies(self) -> None:
        schema = json.loads((ROOT / "assets" / "title_manifest.schema.json").read_text(encoding="utf-8"))
        contract = schema["$defs"]["runtimeContract"]["properties"]
        profile_zero_case = (
            schema["$defs"]["profileZero"]["properties"]["acceptance"]
            ["properties"]["cases"]["items"]["properties"]
        )
        pairs = (
            (
                "runtimeContract.capability_requirements",
                contract["capability_requirements"]["items"]["enum"],
                title_manifest.CORE_CONTRACT_CAPABILITIES,
            ),
            (
                "runtimeContract.hle_overrides[].evidence_class",
                contract["hle_overrides"]["items"]["properties"]["evidence_class"]["enum"],
                title_manifest.EVIDENCE_CLASSES,
            ),
            (
                "profileZero.acceptance.cases[].evidence_class",
                profile_zero_case["evidence_class"]["enum"],
                title_manifest.PROFILE_ZERO_EVIDENCE_CLASSES,
            ),
        )
        for name, published, authoritative in pairs:
            with self.subTest(enum=name):
                self.assertEqual(len(published), len(set(published)), "schema enum has duplicate members")
                self.assertEqual(
                    set(published),
                    set(authoritative),
                    f"schema enum {name} drifted from the Python vocabulary",
                )
        self.assertEqual(
            title_manifest.EVIDENCE_CLASSES - title_manifest.PROFILE_ZERO_EVIDENCE_CLASSES,
            title_manifest.PROFILE_ZERO_FORBIDDEN_EVIDENCE_CLASSES,
        )
        self.assertIn("PRIVATE_TITLE_ACCEPTANCE", title_manifest.PROFILE_ZERO_FORBIDDEN_EVIDENCE_CLASSES)
        self.assertNotIn("PRIVATE_TITLE_ACCEPTANCE", profile_zero_case["evidence_class"]["enum"])

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
        retail.pop("profile_zero", None)
        retail.pop("runtime_contract", None)
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
        with self.assertRaises(title_manifest.TitleManifestError):
            title_manifest.load_manifest(None)
        with self.assertRaises(ValueError):
            title_manifest.load_manifest(self.fixture_path, max_bytes=None)
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

    def test_profile_zero_contract_is_versioned_and_fail_closed(self) -> None:
        normalized = title_manifest.validate_manifest(self.fixture)
        contract = normalized["runtime_contract"]
        self.assertEqual(contract["core_contract"], "psp-core-v1")
        self.assertEqual(contract["unknown_capability_policy"], "fail-closed")
        self.assertEqual(contract["profile_id"], "profile-zero-v1")
        self.assertFalse(normalized["profile_zero"]["acceptance"]["private_inputs_allowed"])
        self.assertEqual(normalized["profile_zero"]["build"]["makefile"], "fixtures/pspdev_phase5/Makefile")

        value = copy.deepcopy(self.fixture)
        value["runtime_contract"]["capability_requirements"].append("unknown-host-fastmem")
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "unknown core capability"):
            title_manifest.validate_manifest(value)

        value = copy.deepcopy(self.fixture)
        value["runtime_contract"]["hle_overrides"] = [{
            "capability": "audio",
            "disposition": "explicit-override",
            "reason": "implicit title behavior",
            "evidence_class": "SOURCE_SHAPE",
        }]
        self.assertEqual(
            title_manifest.validate_manifest(value)["runtime_contract"]["hle_overrides"][0]["capability"],
            "audio",
        )

    def test_profile_zero_cannot_smuggle_private_build_or_input_metadata(self) -> None:
        value = copy.deepcopy(self.fixture)
        value["profile_zero"]["build"]["working_directory"] = "C:/private/game"
        with self.assertRaises(title_manifest.TitleManifestError):
            title_manifest.validate_manifest(value)
        value = copy.deepcopy(self.fixture)
        value["profile_zero"]["build"]["toolchain"] = "C:/private/pspdev"
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "toolchain label"):
            title_manifest.validate_manifest(value)
        value = copy.deepcopy(self.fixture)
        value["profile_zero"]["acceptance"]["private_inputs_allowed"] = True
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "cannot require private"):
            title_manifest.validate_manifest(value)

    def test_profile_zero_rejects_duplicate_sources_private_evidence_and_unsafe_names(self) -> None:
        value = copy.deepcopy(self.fixture)
        value["profile_zero"]["source_program"]["source_files"].append(
            value["profile_zero"]["source_program"]["source_files"][0].upper()
        )
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "duplicate source file"):
            title_manifest.validate_manifest(value)

        value = copy.deepcopy(self.fixture)
        value["profile_zero"]["source_program"]["entry_symbol"] = "main;unsafe"
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "portable C symbol"):
            title_manifest.validate_manifest(value)

        value = copy.deepcopy(self.fixture)
        value["profile_zero"]["build"]["target"] = "all;unsafe"
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "build target name"):
            title_manifest.validate_manifest(value)

        value = copy.deepcopy(self.fixture)
        value["profile_zero"]["acceptance"]["cases"][0]["evidence_class"] = "PRIVATE_TITLE_ACCEPTANCE"
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "private-title evidence"):
            title_manifest.validate_manifest(value)

    def test_profile_zero_runnable_claim_matches_acceptance_state(self) -> None:
        value = copy.deepcopy(self.fixture)
        value["profile_zero"]["runnable"] = True
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "exactly when"):
            title_manifest.validate_manifest(value)

        value = copy.deepcopy(self.fixture)
        value["profile_zero"]["acceptance"]["status"] = "ready"
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "exactly when"):
            title_manifest.validate_manifest(value)


class NativeTitleIdentityTests(unittest.TestCase):
    """Compile/execute the canonical projection using source-owned inputs."""

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="native-title-identity-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.titles = self.root / "assets" / "titles"
        self.titles.mkdir(parents=True)
        shutil.copyfile(ROOT / "assets/public_source_profile.json",
                        self.root / "assets/public_source_profile.json")
        # Stage what the policy includes rather than a hardcoded subset: the
        # policy copied just above names every public manifest.
        self.public_manifests = sorted(
            p.name for p in (ROOT / "assets" / "titles").glob("*.json")
        )
        for name in self.public_manifests:
            shutil.copyfile(ROOT / "assets/titles" / name, self.titles / name)
        self.fixture = title_manifest.load_manifest(self.titles / "synthetic.json")

    def compile_run(self, header: str, body: str) -> bytes:
        compiler = shutil.which(os.environ.get("CC", "gcc"))
        if compiler is None:
            self.fail("native identity regression requires a C compiler (CC or gcc)")
        (self.root / "catalog.h").write_bytes(header.encode("ascii"))
        (self.root / "probe.c").write_bytes(
            ('#include "catalog.h"\n#include <assert.h>\n#include <stdio.h>\n'
             'int main(void) {\n' + body + '\nreturn 0;\n}\n').encode("ascii"))
        exe = self.root / ("probe.exe" if os.name == "nt" else "probe")
        subprocess.run([compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                        str(self.root / "probe.c"), "-o", str(exe)],
                       check=True, capture_output=True, timeout=60)
        return subprocess.run([str(exe)], check=True, capture_output=True, timeout=10).stdout

    def retail(self, title_id: str = "retail-fixture") -> dict:
        value = copy.deepcopy(self.fixture)
        value.pop("profile_zero", None)
        value["kind"] = "retail"
        value["id"] = title_id
        value["disc"] = {"id": "TEST12345", "region": "NA",
                         "revision_policy": "explicit-compatible-revisions",
                         "compatible_revisions": ["TEST12346"]}
        return value

    def test_native_strings_and_identity_lookups_preserve_the_manifest(self) -> None:
        value = self.retail()
        value["display_name"] = 'Path\\backslash "quote" café 日本 ??/A'
        second = title_manifest.load_manifest(self.titles / "synthetic-title2.json")
        header = title_manifest.render_native_title_catalog([value, second])
        actual = self.compile_run(header, r'''
const NkTitleIdentity *title = nk_title_identity_by_id("retail-fixture");
assert(NK_TITLE_IDENTITY_COUNT == 2);
assert(title && title->disc_id_count == 2);
assert(nk_title_identity_by_disc("TEST12345") == title);
assert(nk_title_identity_by_disc("TEST12346") == title);
assert(!nk_title_identity_by_disc("TEST12344"));
assert(!nk_title_identity_by_disc("TEST12347"));
assert(!nk_title_identity_by_disc("TEST1234"));
assert(!nk_title_identity_by_disc("TEST123450"));
assert(!nk_title_identity_by_disc("test12345"));
assert(!nk_title_identity_by_disc("TEST-12345"));
assert(!nk_title_identity_by_disc("TEST00002"));
assert(!nk_title_identity_by_disc(NULL));
assert(!nk_title_identity_by_id(NULL));
assert(!nk_title_identity_by_id("retail-fixture-extra"));
assert(!nk_title_identity_by_id(""));
const NkTitleIdentity *synthetic = nk_title_identity_by_id("synthetic-title2-v1");
assert(synthetic && synthetic->disc_id_count == 0 && !synthetic->disc_ids);
assert(strlen(title->manifest_sha256) == 64);
fputs(title->display_name, stdout);
''')
        self.assertEqual(actual, value["display_name"].encode("utf-8"))

    def test_public_projection_compiles_and_has_no_inferred_disc_ids(self) -> None:
        header = title_manifest.public_native_title_catalog(self.root)
        # Derived, not a literal.
        count_check = "assert(NK_TITLE_IDENTITY_COUNT == %d);" % len(self.public_manifests)
        self.compile_run(header, count_check + r'''
assert(nk_title_identity_by_id("synthetic-allegrex-v1"));
assert(nk_title_identity_by_id("synthetic-title2-v1"));
assert(nk_title_identity_by_id("pspdev-phase5-v1"));
assert(!nk_title_identity_by_disc("TEST00001"));
assert(!nk_title_identity_by_disc("TEST00002"));
assert(!nk_title_identity_by_disc("TEST00005"));
''')

    def test_empty_catalog_is_valid_c_and_never_matches(self) -> None:
        self.compile_run(title_manifest.render_native_title_catalog([]), r'''
assert(NK_TITLE_IDENTITY_COUNT == 0);
assert(!nk_title_identity_by_id("anything"));
assert(!nk_title_identity_by_disc("TEST12345"));
''')

    def test_duplicate_title_or_disc_identities_are_rejected(self) -> None:
        first = self.retail()
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "duplicate title"):
            title_manifest.render_native_title_catalog([first, first])
        second = self.retail("another-title")
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "duplicate disc"):
            title_manifest.render_native_title_catalog([first, second])
        second["disc"] = {"id": "TEST12346", "region": "NA",
                          "revision_policy": "exact-disc-id"}
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "duplicate disc"):
            title_manifest.render_native_title_catalog([first, second])

    def test_private_and_unclassified_files_are_not_read(self) -> None:
        before = title_manifest.public_native_title_catalog(self.root)
        for name in ("hst-ucus98701.json", "unclassified.json"):
            (self.titles / name).write_bytes(b"not JSON: private fixture marker")
        self.assertEqual(before, title_manifest.public_native_title_catalog(self.root))
        self.assertNotIn("private fixture marker", before)

    def test_missing_or_malformed_included_input_fails_closed(self) -> None:
        path = self.titles / "synthetic.json"
        path.write_bytes(b'{"schema_version":1,"schema_version":1}')
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "duplicate JSON"):
            title_manifest.public_native_title_catalog(self.root)
        path.unlink()
        with self.assertRaises((title_manifest.TitleManifestError, OSError)):
            title_manifest.public_native_title_catalog(self.root)

    def test_included_alias_is_rejected_without_reading_target(self) -> None:
        path = self.titles / "synthetic.json"
        target = self.root / "external.json"
        target.write_bytes(b"private alias target")
        path.unlink()
        try:
            path.symlink_to(target)
        except OSError as exc:
            self.skipTest(f"host cannot create symlink: {exc}")
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "symbolic links"):
            title_manifest.public_native_title_catalog(self.root)

    def test_canonical_order_and_formatting_do_not_change_output(self) -> None:
        other = title_manifest.load_manifest(self.titles / "synthetic-title2.json")
        self.assertEqual(title_manifest.render_native_title_catalog([self.fixture, other]),
                         title_manifest.render_native_title_catalog([other, self.fixture]))
        before = title_manifest.public_native_title_catalog(self.root)
        (self.titles / "synthetic.json").write_bytes(
            json.dumps(self.fixture, indent=4).encode("utf-8"))
        self.assertEqual(before, title_manifest.public_native_title_catalog(self.root))
        self.fixture["executable"]["entry"] += 4
        (self.titles / "synthetic.json").write_bytes(json.dumps(self.fixture).encode("utf-8"))
        self.assertNotEqual(before, title_manifest.public_native_title_catalog(self.root),
                            "digest must bind even non-projected contract fields")

    def test_malformed_schema_is_rejected_before_projection(self) -> None:
        value = copy.deepcopy(self.fixture)
        value["unrecognized"] = True
        with self.assertRaisesRegex(title_manifest.TitleManifestError, "unknown field"):
            title_manifest.render_native_title_catalog([value])

    def test_cli_emits_only_generated_ascii_and_preserves_old_mode(self) -> None:
        script = ROOT / "tools/title_manifest.py"
        result = subprocess.run([sys.executable, str(script), "--print-public-catalog"],
                                capture_output=True, check=True, timeout=15)
        self.assertEqual(result.stdout, title_manifest.public_native_title_catalog(ROOT).encode("ascii"))
        self.assertEqual(result.stderr, b"")
        result = subprocess.run([sys.executable, str(script), str(self.titles / "synthetic.json")],
                                capture_output=True, check=True, timeout=15)
        self.assertIn(b"OK: synthetic-allegrex-v1", result.stdout)
        result = subprocess.run([sys.executable, str(script), "--print-public-catalog",
                                 str(self.titles / "synthetic.json")], capture_output=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b"")


if __name__ == "__main__":
    unittest.main()
