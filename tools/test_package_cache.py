# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import publication_policy
import publish_audit
import nk_cli
import title_codegen_plan
from nk_core import package_cache
from nk_doctor_checks import check_runtime_package_cache
from nk_doctor_core import Report


class PackageCacheTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="package_cache_")
        self.root = Path(self.temp.name)
        self.inputs = {
            "manifest": {"sha256": "1" * 64},
            "executable": {"sha256": "2" * 64},
            "modules": [],
            "psp_header": None,
        }

    def tearDown(self) -> None:
        self.temp.cleanup()

    def key(self, **overrides):
        values = {
            "input_hashes": self.inputs,
            "codegen_options": {"profile": "none", "funcs_per_chunk": 2000},
            "analyzer_sha256": "3" * 64,
            "codegen_sha256": "4" * 64,
            "compiler": "gcc:test",
            "target": "x86_64-test",
            "runtime_source_digest": "5" * 64,
            "compile_flags": "-O0",
            "link_flags": "-lm",
        }
        values.update(overrides)
        return package_cache.build_cache_key(**values)

    def test_cache_decision_names_each_invalidation_tier(self) -> None:
        current = self.key()
        self.assertEqual(package_cache.compare_cache_keys(current, current).action, "reuse")

        changed_inputs = dict(self.inputs)
        changed_inputs["executable"] = {"sha256": "6" * 64}
        executable_change = package_cache.compare_cache_keys(
            current, self.key(input_hashes=changed_inputs)
        )
        self.assertEqual(executable_change.action, "aot-regenerate")
        self.assertIn("aot:executable_sha256", executable_change.reasons)

        options_change = package_cache.compare_cache_keys(
            current, self.key(codegen_options={"profile": "none", "funcs_per_chunk": 1})
        )
        self.assertEqual(options_change.action, "aot-regenerate")
        self.assertIn("aot:codegen_options_sha256", options_change.reasons)

        analyzer_change = package_cache.compare_cache_keys(
            current, self.key(analyzer_sha256="7" * 64)
        )
        self.assertEqual(analyzer_change.action, "aot-regenerate")
        self.assertIn("aot:analyzer_sha256", analyzer_change.reasons)

        abi_change = package_cache.compare_cache_keys(
            current, self.key(generated_code_abi_epoch=2)
        )
        self.assertEqual(abi_change.action, "aot-regenerate")
        self.assertIn("aot:generated_code_abi_epoch", abi_change.reasons)

        compiler_change = package_cache.compare_cache_keys(
            current, self.key(compiler="gcc:other")
        )
        self.assertEqual(compiler_change.action, "native-recompile")
        self.assertTrue(compiler_change.generated_c_reusable)
        self.assertFalse(compiler_change.native_objects_reusable)
        self.assertIn("native:compiler_identity", compiler_change.reasons)

        runtime_change = package_cache.compare_cache_keys(
            current, self.key(runtime_source_digest="8" * 64)
        )
        self.assertEqual(runtime_change.action, "native-recompile")
        self.assertIn("native:runtime_source_digest", runtime_change.reasons)

        trace_change = package_cache.compare_cache_keys(
            current,
            self.key(compile_flags=package_cache.native_compile_flags(
                environment={"TRACE": "1"}
            )),
        )
        self.assertEqual(trace_change.action, "native-recompile")
        public_safe_change = package_cache.compare_cache_keys(
            current,
            self.key(compile_flags=package_cache.native_compile_flags(public_safe=True)),
        )
        self.assertEqual(public_safe_change.action, "native-recompile")

        runtime_v2 = self.key(runtime_abi_epoch=2)
        self.assertEqual(
            package_cache.compare_cache_keys(current, runtime_v2).action,
            "aot-regenerate",
        )
        with mock.patch.dict(
            package_cache.RUNTIME_ABI_COMPATIBILITY,
            {1: frozenset({1, 2})},
            clear=True,
        ):
            compatible = package_cache.compare_cache_keys(current, runtime_v2)
        self.assertEqual(compatible.action, "native-recompile")
        self.assertTrue(compatible.generated_c_reusable)
        self.assertIn("aot:runtime_abi_epoch", compatible.reasons)

    def test_cache_key_differs_by_public_safe_mode(self) -> None:
        key_public = self.key(compile_flags=package_cache.native_compile_flags(public_safe=True))
        key_private = self.key(compile_flags=package_cache.native_compile_flags(public_safe=False))
        self.assertNotEqual(key_public["native"]["digest"], key_private["native"]["digest"])
        decision = package_cache.compare_cache_keys(key_public, key_private)
        self.assertEqual(decision.action, "native-recompile")
        self.assertIn("native:compile_flags", decision.reasons)
        self.assertTrue(decision.generated_c_reusable)

        manifest_path = self.root / "manifest.json"
        manifest_path.write_text("{}", encoding="utf-8")
        manifest = {"modules": [], "executable": {"base": 0x08804000, "entry": 0x08804000}}
        cli_key_pub = nk_cli._current_package_cache_key(
            manifest,
            manifest_path,
            "2" * 64,
            None,
            None,
            public_safe=True,
        )
        cli_key_priv = nk_cli._current_package_cache_key(
            manifest,
            manifest_path,
            "2" * 64,
            None,
            None,
            public_safe=False,
        )
        self.assertNotEqual(
            cli_key_pub["native"]["digest"],
            cli_key_priv["native"]["digest"],
        )

    def test_completion_manifest_rejects_interruption_and_corruption(self) -> None:
        package_dir = self.root / "package"
        package_dir.mkdir()
        executable = package_dir / "synthetic.exe"
        image = package_dir / "synthetic_image.bin"
        generated = package_dir / "synthetic_recomp.o"
        executable.write_bytes(b"native")
        image.write_bytes(b"image")
        generated.write_bytes(b"object")
        key = self.key()
        cache = package_cache.cache_metadata(
            key, {"profile": "none", "funcs_per_chunk": 2000}
        )
        package = {
            "cache": cache,
            "inputs": self.inputs,
            "executable": {
                "path": executable.name,
                "sha256": package_cache.sha256_file(executable),
            },
            "generated_objects": [{
                "path": generated.name,
                "sha256": package_cache.sha256_file(generated),
            }],
        }
        (package_dir / "package.json").write_text(
            package_cache.canonical_json(package), encoding="utf-8"
        )
        (package_dir / "build-report.json").write_text(
            package_cache.canonical_json({"cache": cache}), encoding="utf-8"
        )
        package_cache.write_completion_manifest(package_dir, key)
        valid, reason = package_cache.validate_package_cache(package_dir, expected_key=key)
        self.assertTrue(valid, reason)
        user_root = self.root / "user-data"
        published = user_root / "packages" / "TEST00001"
        shutil.copytree(package_dir, published)
        report = Report(self.root, "products")
        check_runtime_package_cache(
            report,
            user_root,
            "TEST00001",
            self.key(compiler="gcc:other"),
        )
        self.assertTrue(any(result.detail and "native:compiler_identity" in result.detail
                            for result in report.results))

        completion_path = package_dir / package_cache.COMPLETION_MANIFEST
        completion = json.loads(completion_path.read_text(encoding="utf-8"))
        completion["artifacts"][0]["sha256"] = "0" * 64
        completion_path.write_text(
            package_cache.canonical_json(completion), encoding="utf-8"
        )
        valid, reason = package_cache.validate_package_cache(package_dir, expected_key=key)
        self.assertFalse(valid)
        self.assertIn("digest mismatch", reason)

        completion_path.unlink()
        valid, reason = package_cache.validate_package_cache(package_dir, expected_key=key)
        self.assertFalse(valid)
        self.assertIn("missing", reason)

        interrupted = self.root / "interrupted"
        interrupted.mkdir()
        (interrupted / "package.json").write_text("{}", encoding="utf-8")
        valid, reason = package_cache.validate_package_cache(interrupted, expected_key=key)
        self.assertFalse(valid)
        self.assertIn("unreadable", reason)

    def test_cli_and_planner_compute_the_same_key(self) -> None:
        manifest = json.loads(
            (ROOT / "assets" / "titles" / "synthetic.json").read_text(encoding="utf-8")
        )
        manifest_path = self.root / "manifest.json"
        manifest_path.write_text(
            title_codegen_plan.canonical_json(manifest), encoding="utf-8"
        )
        elf_path = self.root / "synthetic.elf"
        elf_path.write_bytes(b"synthetic elf")
        executable_hash = package_cache.sha256_file(elf_path)
        plan = title_codegen_plan.build_plan(
            manifest,
            game_name="synthetic",
            game_elf=elf_path,
            build_dir=self.root / "build",
            codegen_profile="none",
        )
        input_hashes = title_codegen_plan._hash_package_inputs(
            manifest_path, elf_path, [], {}, None
        )
        environment = dict(os.environ)
        for mode in (None, True, False):
            with mock.patch.object(
                nk_cli, "_runtime_build_environment", return_value=environment
            ):
                cli_key = nk_cli._current_package_cache_key(
                    manifest, manifest_path, executable_hash, None, None, public_safe=mode
                )
            planner_key = title_codegen_plan._cache_key_for_build(
                input_hashes=input_hashes,
                plan=plan,
                selected_optional=set(),
                funcs_per_chunk=2000,
                public_safe=mode,
                compiler_name=environment.get("CC", "gcc"),
            )
            self.assertEqual(cli_key, planner_key)

    def test_promotion_refuses_a_copy_that_fails_validation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            user_root = Path(tmp).resolve()
            cache_dir = user_root / "cache"
            entry_package = cache_dir / "entries" / "a" / "b" / "package"
            entry_package.mkdir(parents=True)
            (entry_package / "package.json").write_text("{}", encoding="utf-8")
            target_dir = user_root / "packages" / "ABCD12345"
            target_dir.mkdir(parents=True)
            (target_dir / "package.json").write_text('{"previous": true}', encoding="utf-8")
            with mock.patch.object(
                package_cache, "validate_package_cache", return_value=(False, "digest mismatch")
            ):
                with self.assertRaisesRegex(nk_cli.PackageBuildError, "digest mismatch"):
                    nk_cli._promote_cache_entry(
                        entry_package, target_dir, cache_dir, {}, user_root, "ABCD12345"
                    )
            # The existing package stays in place and no staging directory is left behind.
            self.assertEqual(
                (target_dir / "package.json").read_text(encoding="utf-8"), '{"previous": true}'
            )
            self.assertEqual(
                [p.name for p in target_dir.parent.iterdir()], ["ABCD12345"]
            )

    def test_cache_cleanup_is_bounded_and_preserves_protected_entry(self) -> None:
        cache_root = package_cache.cache_root(self.root / "user-data", "TEST00001")
        entries_root = cache_root / "entries"
        aot_root = entries_root / ("a" * 64)
        entries = []
        for index, digest in enumerate(("1" * 64, "2" * 64, "3" * 64)):
            entry = aot_root / digest
            (entry / "package").mkdir(parents=True)
            os.utime(entry, (index + 1, index + 1))
            entries.append(entry)
        removed, remaining = package_cache.prune_cache(
            cache_root,
            max_entries=2,
            protected_entry=entries[0],
        )
        self.assertEqual(removed, 1)
        self.assertEqual(remaining, 2)
        self.assertTrue(entries[0].is_dir())
        self.assertFalse(entries[1].is_dir())
        self.assertTrue(entries[2].is_dir())
        with self.assertRaises(package_cache.PackageCacheError):
            package_cache.cache_entry_limit({package_cache.CACHE_LIMIT_ENV: "0"})

    def test_cache_root_is_private_and_public_scope_rejects_tracked_cache(self) -> None:
        user_root = self.root / "user-data"
        cache = package_cache.cache_root(user_root, "test00001")
        self.assertTrue(cache.is_relative_to(user_root.resolve()))
        self.assertTrue(package_cache.cache_root_is_private(user_root, ROOT))
        self.assertFalse(package_cache.cache_root_is_private(ROOT / "cache", ROOT))
        self.assertIsNotNone(publish_audit._forbidden_path(
            "cache/packages/TEST00001/entries/key/package.json"
        ))

        repo = self.root / "repo"
        repo.mkdir()
        policy_path = repo / "policy.json"
        export_path = repo / "export.json"
        policy = {
            "name": "cache-test",
            "profile_version": "2.0.0",
            "min_tool_version": "0.4.0",
            "build_mode": "PUBLIC_SAFE=1",
            "default_disposition": "REJECT",
            "exclude_prefixes": [],
            "exclude_globs": [],
            "exclude_paths": [],
            "include_paths": [],
        }
        policy_path.write_text(json.dumps(policy), encoding="utf-8")
        export_path.write_text(
            json.dumps({
                "profile": policy["name"],
                "policy_sha256": publication_policy.canonical_digest(policy),
            }) + "\n",
            encoding="utf-8",
        )
        entry = publish_audit.GitEntry(
            "100644", "hash", "0", "cache/packages/TEST00001/package.json", "file"
        )
        findings = publish_audit.audit_entries(
            [entry],
            repo_root=repo,
            is_candidate_root=True,
            public_scope=True,
            policy_path=policy_path,
            export_path=export_path,
        )
        self.assertTrue(findings)
        self.assertTrue(any(finding.code in {"UNRESOLVED_PUBLIC", "POLICY_UNCLASSIFIED"} for finding in findings))


if __name__ == "__main__":
    unittest.main()
