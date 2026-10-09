# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import ast
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
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


def _aot_gap_report(cache: dict, region_count: int) -> str:
    """A synthetic build-report body in the planner's own AOT-gap record shape.

    Records, not filler text: the planner writes one record per AOT gap, so a
    report that grows with the title also grows proportionally in structural
    nodes and comma separators. 6000 records is about 1.3 MB, the measured
    size of a flagship build report, so the fixture reproduces that route.
    """
    return package_cache.canonical_json({
        "cache": cache,
        "unsupported": {
            "regions": [
                {
                    "boundary": f"AOT function at 0x{index:08x}",
                    "entry_address": f"0x{index:08x}",
                    "module": "executable region",
                    "reason": "no analyzed AOT span; interpreted by the AOT-gap floor (#118)",
                    "status": "in the works",
                    "tracking_issue": 118,
                }
                for index in range(region_count)
            ]
        },
    })


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
        self.identity = package_cache.build_title_input_identity(
            manifest={"id": "synthetic-cache-test", "schema_version": 1},
            executable_name="EBOOT.BIN",
            executable_sha256="2" * 64,
            modules=[],
            disc_id="TEST00001",
            region="TEST",
            disc_version="1.00",
            param_sfo={"DISC_ID": "TEST00001", "DISC_VERSION": "1.00"},
            container={
                "format": "iso9660", "volume_id": "CACHE_TEST",
                "size_bytes": 2048, "pvd_sector": 16, "sector_size": 2048,
            },
        )

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_native_build_report_byte_ceiling_matches_python(self) -> None:
        header = (ROOT / "src/core/nk_title_manifest.h").read_text(
            encoding="utf-8"
        )
        match = re.search(
            r"(?m)^\s*#\s*define\s+NK_BUILD_REPORT_MAX_BYTES\s+\(\s*(\d+)\s*\*\s*1024\s*\*\s*1024\s*\)\s*$",
            header,
        )
        self.assertIsNotNone(match)
        assert match is not None
        native_limit = int(match.group(1)) * 1024 * 1024
        self.assertEqual(native_limit, package_cache.MAX_BUILD_REPORT_JSON_BYTES)

    def key(self, **overrides):
        values = {
            "input_hashes": self.inputs,
            "title_input_identity": self.identity,
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

    def _new_package_dir(self, name: str) -> tuple[Path, dict, dict]:
        """Create a package holding everything except its report and manifest.

        Returns the package directory, the cache key the caller must pass to
        validate_package_cache(), and the cache metadata both package.json and
        build-report.json must carry.
        """
        package_dir = self.root / name
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
            "format": "nakagawa-aot-package",
            "schema_version": 2,
            "title": {
                "id": self.identity["manifest"]["id"],
                "manifest_sha256": self.inputs["manifest"]["sha256"],
            },
            "title_input_identity": self.identity,
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
        return package_dir, key, cache

    def test_retail_input_identity_cannot_be_written_inside_checkout(self) -> None:
        with self.assertRaisesRegex(
            title_codegen_plan.PackageRouteError, "per-user data root"
        ):
            title_codegen_plan._ensure_private_identity_output(
                {"kind": "retail"}, ROOT / "build" / "local-package"
            )
        title_codegen_plan._ensure_private_identity_output(
            {"kind": "synthetic"}, ROOT / "build" / "synthetic-package"
        )

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

        changed_module = {"name": "fixture.prx", "sha256": "6" * 64}
        changed_modules = [changed_module]
        changed_module_identity = package_cache.build_title_input_identity(
            manifest={"id": "synthetic-cache-test", "schema_version": 1},
            executable_name="EBOOT.BIN",
            executable_sha256="2" * 64,
            modules=changed_modules,
            disc_id="TEST00001",
            region="TEST",
            disc_version="1.00",
            param_sfo={"DISC_ID": "TEST00001", "DISC_VERSION": "1.00"},
            container=self.identity["container"],
        )
        changed_module_inputs = dict(self.inputs, modules=[{
            "name": "fixture.prx", "load_address": "0x08810000", "sha256": "6" * 64,
        }])
        module_change = package_cache.compare_cache_keys(
            current,
            self.key(
                input_hashes=changed_module_inputs,
                title_input_identity=changed_module_identity,
            ),
        )
        self.assertEqual(module_change.action, "aot-regenerate")
        self.assertIn("aot:modules_sha256", module_change.reasons)
        self.assertIn("aot:title_input_identity_sha256", module_change.reasons)
        self.assertEqual(
            package_cache.title_input_identity_changes(self.identity, changed_module_identity),
            ("module fixture.prx changed",),
        )

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

    def test_title_input_identity_binds_source_media_revisions(self) -> None:
        source_media = {
            "executable": {
                "path": "PSP_GAME/SYSDIR/EBOOT.BIN",
                "sha256": "6" * 64,
            },
            "modules": [{
                "name": "fixture.prx",
                "path": "PSP_GAME/USRDIR/fixture.prx",
                "sha256": "7" * 64,
            }],
        }
        original = package_cache.build_title_input_identity(
            manifest={"id": "synthetic-cache-test", "schema_version": 1},
            executable_name="EBOOT.BIN",
            executable_sha256="2" * 64,
            modules=[{"name": "fixture.prx", "sha256": "3" * 64}],
            source_media=source_media,
        )
        self.assertEqual(package_cache.validate_title_input_identity(original), original)
        changed_source = dict(source_media)
        changed_source["executable"] = {
            "path": "PSP_GAME/SYSDIR/EBOOT.BIN",
            "sha256": "8" * 64,
        }
        changed = package_cache.build_title_input_identity(
            manifest={"id": "synthetic-cache-test", "schema_version": 1},
            executable_name="EBOOT.BIN",
            executable_sha256="2" * 64,
            modules=[{"name": "fixture.prx", "sha256": "3" * 64}],
            source_media=changed_source,
        )
        self.assertIn(
            "source executable changed",
            package_cache.title_input_identity_changes(original, changed),
        )
        invalid = dict(original)
        invalid["source_media"] = {
            "executable": {
                "path": "PSP_GAME/SYSDIR/../EBOOT.BIN",
                "sha256": "6" * 64,
            },
            "modules": [],
        }
        with self.assertRaisesRegex(package_cache.PackageCacheError, "source executable path"):
            package_cache.validate_title_input_identity(invalid)

    def test_source_iso_paths_match_native_validator_table(self) -> None:
        native_test = (ROOT / "tests/native/test_player_state.c").read_text(
            encoding="utf-8"
        )
        table = re.search(
            r"(?ms)^\s*\} source_iso_path_cases\[\] = \{(?P<cases>.*?)^\};",
            native_test,
        )
        self.assertIsNotNone(table)
        cases = re.findall(
            r'^\s*\{"((?:\\.|[^"\\])*)",\s*(true|false)\},?\s*$',
            table.group("cases"),
            re.MULTILINE,
        )
        self.assertGreaterEqual(len(cases), 20)
        for encoded_path, expected_text in cases:
            with self.subTest(path=encoded_path):
                path = ast.literal_eval(f'"{encoded_path}"')
                expected_valid = expected_text == "true"
                if expected_valid:
                    self.assertEqual(
                        package_cache._validate_source_iso_path(path, "test"), path
                    )
                else:
                    with self.assertRaises(package_cache.PackageCacheError):
                        package_cache._validate_source_iso_path(path, "test")

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
            package_cache.build_title_input_identity(
                manifest={"id": "synthetic-cache-test", "schema_version": 1},
                executable_name="EBOOT.BIN",
                executable_sha256="2" * 64,
                modules=[],
            ),
            public_safe=True,
        )
        cli_key_priv = nk_cli._current_package_cache_key(
            manifest,
            manifest_path,
            "2" * 64,
            None,
            None,
            package_cache.build_title_input_identity(
                manifest={"id": "synthetic-cache-test", "schema_version": 1},
                executable_name="EBOOT.BIN",
                executable_sha256="2" * 64,
                modules=[],
            ),
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
            "format": "nakagawa-aot-package",
            "schema_version": 2,
            "title": {
                "id": self.identity["manifest"]["id"],
                "manifest_sha256": self.inputs["manifest"]["sha256"],
            },
            "title_input_identity": self.identity,
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
        package_cache.write_local_title_input_identity(user_root, self.identity)
        report = Report(self.root, "products")
        check_runtime_package_cache(
            report,
            user_root,
            "TEST00001",
            self.key(compiler="gcc:other"),
        )
        self.assertTrue(any(result.detail and "native:compiler_identity" in result.detail
                            for result in report.results))

        changed_identity = dict(self.identity)
        changed_identity["main_executable"] = {
            "name": "EBOOT.BIN", "sha256": "9" * 64,
        }
        package_cache.write_local_title_input_identity(user_root, changed_identity)
        mismatch_report = Report(self.root, "products")
        check_runtime_package_cache(mismatch_report, user_root, "TEST00001")
        mismatch = next(
            item for item in mismatch_report.results
            if item.code == "RUNTIME_PACKAGE_CACHE"
        )
        self.assertEqual(mismatch.status, "FAIL")
        self.assertIn("main executable changed", mismatch.detail or "")
        self.assertIn("built from different local inputs", mismatch.detail or "")

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

    def test_flagship_sized_build_report_is_accepted(self) -> None:
        # A flagship build report is about 1.3 MB, past the shared 1 MiB byte
        # ceiling, so the package route rejected it with PACKAGE_BUILD_INCOMPLETE
        # even though the report was produced by this project's own planner.
        package_dir, key, cache = self._new_package_dir("flagship_report")
        report = _aot_gap_report(cache, 6000)
        size = len(report.encode("utf-8"))
        self.assertGreater(size, package_cache.MAX_CACHE_JSON_BYTES)
        self.assertLess(size, package_cache.MAX_BUILD_REPORT_JSON_BYTES)
        (package_dir / "build-report.json").write_text(report, encoding="utf-8")
        package_cache.write_completion_manifest(package_dir, key)
        valid, reason = package_cache.validate_package_cache(package_dir, expected_key=key)
        self.assertTrue(valid, reason)

    def test_build_report_byte_ceiling_accepts_the_limit_and_rejects_one_over(self) -> None:
        limit = package_cache.MAX_BUILD_REPORT_JSON_BYTES
        package_dir, key, cache = self._new_package_dir("report_byte_ceiling")
        report_path = package_dir / "build-report.json"
        prefix = '{"cache":' + package_cache.canonical_json(cache).strip() + ',"pad":"'
        at_limit = prefix + "A" * (limit - len(prefix) - 2) + '"}'
        self.assertEqual(len(at_limit.encode("utf-8")), limit)
        report_path.write_text(at_limit, encoding="utf-8")
        package_cache.write_completion_manifest(package_dir, key)
        valid, reason = package_cache.validate_package_cache(package_dir, expected_key=key)
        self.assertTrue(valid, reason)  # valid data at the exact limit is accepted
        over_limit = prefix + "A" * (limit + 1 - len(prefix) - 2) + '"}'
        self.assertEqual(len(over_limit.encode("utf-8")), limit + 1)
        report_path.write_text(over_limit, encoding="utf-8")
        with self.assertRaisesRegex(
            package_cache.BoundedJsonError, "4194304-byte JSON artifact limit"
        ):
            package_cache.read_build_report_json(report_path)
        valid, reason = package_cache.validate_package_cache(package_dir)
        self.assertFalse(valid)
        self.assertIn("over the 4194304-byte JSON artifact limit", reason)

    def test_package_json_keeps_the_shared_one_mib_ceiling(self) -> None:
        # Only build-report.json may be large: package.json, the completion
        # manifest and the identity record keep the shared 1 MiB bound.
        package_dir, key, cache = self._new_package_dir("oversize_package_json")
        (package_dir / "build-report.json").write_text(
            _aot_gap_report(cache, 8), encoding="utf-8"
        )
        package_path = package_dir / "package.json"
        document = json.loads(package_path.read_text(encoding="utf-8"))
        document["pad"] = "A" * package_cache.MAX_CACHE_JSON_BYTES
        package_path.write_text(
            package_cache.canonical_json(document), encoding="utf-8"
        )
        valid, reason = package_cache.validate_package_cache(package_dir, expected_key=key)
        self.assertFalse(valid)
        self.assertIn("over the 1048576-byte JSON artifact limit", reason)

    def test_planner_refuses_a_build_report_the_package_reader_would_reject(self) -> None:
        small = {"format": "nakagawa-build-report", "schema_version": 1}
        self.assertEqual(
            title_codegen_plan._build_report_text(small),
            package_cache.canonical_json(small),
        )
        oversized = {"pad": "A" * package_cache.MAX_BUILD_REPORT_JSON_BYTES}
        with self.assertRaises(title_codegen_plan.PackageRouteError) as caught:
            title_codegen_plan._build_report_text(oversized)
        self.assertEqual(caught.exception.code, "PACKAGE_REPORT_TOO_LARGE")
        self.assertIn("4194304-byte build report limit", str(caught.exception))

    def test_planner_refuses_a_build_report_over_the_node_ceiling(self) -> None:
        rows = [{"nid": i, "kind": "aot_gap", "site": 1} for i in range(70000)]
        report = {
            "schema": title_codegen_plan.BUILD_REPORT_FORMAT,
            "unsupported": {"imports": rows},
        }
        self.assertLess(len(title_codegen_plan.canonical_json(report).encode()), 4 * 1024 * 1024)
        with self.assertRaises(title_codegen_plan.PackageRouteError) as caught:
            title_codegen_plan._build_report_text(report)
        self.assertEqual(caught.exception.code, "PACKAGE_REPORT_TOO_LARGE")
        self.assertIn("rejected by the package reader", str(caught.exception))

    def test_completion_manifest_accepts_gcc_runtime_dll_artifact_names(self) -> None:
        # Host runtime closure DLLs ship inside packages and GCC/MSYS2 library
        # names contain '+' (libstdc++-6.dll). The artifact rule must mirror
        # is_valid_artifact_path() in src/core/nk_title_manifest.c so the
        # completion manifest never records what the native validator rejects.
        package_dir = self.root / "package"
        package_dir.mkdir()
        executable = package_dir / "synthetic.exe"
        image = package_dir / "synthetic_image.bin"
        generated = package_dir / "synthetic_recomp.o"
        runtime_dll = package_dir / "libstdc++-6.dll"
        notices = package_dir / "THIRD_PARTY_LICENSES"
        notices.mkdir()
        notice = notices / "Sdl3_ttf-zlib.txt"
        executable.write_bytes(b"native")
        image.write_bytes(b"image")
        generated.write_bytes(b"object")
        runtime_dll.write_bytes(b"dll")
        notice.write_text("licence text\n", encoding="utf-8")
        key = self.key()
        cache = package_cache.cache_metadata(
            key, {"profile": "none", "funcs_per_chunk": 2000}
        )
        package = {
            "format": "nakagawa-aot-package",
            "schema_version": 2,
            "title": {
                "id": self.identity["manifest"]["id"],
                "manifest_sha256": self.inputs["manifest"]["sha256"],
            },
            "title_input_identity": self.identity,
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
        valid, reason, _ = package_cache.validate_completion_manifest(
            package_dir, expected_key=key
        )
        self.assertTrue(valid, reason)
        manifest = json.loads(
            (package_dir / package_cache.COMPLETION_MANIFEST).read_text(
                encoding="utf-8"
            )
        )
        recorded = {record["path"] for record in manifest["artifacts"]}
        self.assertIn("libstdc++-6.dll", recorded)
        self.assertIn("THIRD_PARTY_LICENSES/Sdl3_ttf-zlib.txt", recorded)

        for good in ("libstdc++-6.dll", "libSDL3_ttf.so.0", "dir/libc++.so"):
            self.assertEqual(package_cache._safe_relative(good), good)
        for bad in (
            "..",
            "dir/../escape.dll",
            "back\\slash.dll",
            "C:/abs.dll",
            "/abs.dll",
            "CON.dll",
            "dir//empty.dll",
            "trailing.",
            "spa ce.dll",
            "semi;colon.dll",
        ):
            with self.subTest(path=bad):
                with self.assertRaises(package_cache.PackageCacheError):
                    package_cache._safe_relative(bad)

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
        identity = package_cache.build_title_input_identity(
            manifest=manifest,
            executable_name="synthetic.elf",
            executable_sha256=executable_hash,
            modules=[],
        )
        self.assertEqual(package_cache.validate_title_input_identity(identity), identity)
        environment = dict(os.environ)
        for mode in (None, True, False):
            with mock.patch.object(
                nk_cli, "_runtime_build_environment", return_value=environment
            ):
                cli_key = nk_cli._current_package_cache_key(
                    manifest, manifest_path, executable_hash, None, None, public_safe=mode
                    , title_input_identity=identity
                )
            planner_key = title_codegen_plan._cache_key_for_build(
                input_hashes=input_hashes,
                title_input_identity=identity,
                plan=plan,
                selected_optional=set(),
                funcs_per_chunk=2000,
                public_safe=mode,
                compiler_name=environment.get("CC", "gcc"),
            )
            self.assertEqual(cli_key, planner_key)

    def test_compiler_target_is_the_c_compiler_not_the_planning_interpreter(self) -> None:
        # The native objects are built by CC, so the key must name that compiler's
        # target. The planner and the CLI can run under different interpreters (the
        # player's MSYS2 python and the Python on PATH report different sysconfig
        # platforms for the same gcc), so an interpreter-derived target made the native
        # key change between two builds of the same inputs.
        environment = {key: value for key, value in os.environ.items()
                       if key not in ("NK_TARGET_TRIPLE", "CC_TARGET")}
        gcc = shutil.which("gcc", path=environment.get("PATH"))
        if gcc is None:
            self.skipTest("gcc is not on PATH, so there is no compiler target to read")
        expected = subprocess.run(
            [gcc, "-dumpmachine"], capture_output=True, text=True, check=True,
        ).stdout.strip()
        self.assertTrue(expected)
        # The interpreter's platform is varied on purpose and must not reach the key: the
        # mocks are asserted to be unconsulted, so a future regression that reads them
        # fails here instead of silently passing with the value unchanged.
        targets = set()
        for machine, interpreter_platform in (("AMD64", "win-amd64"),
                                              ("AMD64", "mingw_x86_64_ucrt_gnu"),
                                              ("x86_64", "linux-x86_64")):
            with mock.patch("platform.machine", return_value=machine) as machine_probe, \
                    mock.patch("sysconfig.get_platform",
                               return_value=interpreter_platform) as platform_probe:
                targets.add(package_cache.compiler_target(environment))
            machine_probe.assert_not_called()
            platform_probe.assert_not_called()
        self.assertEqual(targets, {expected})
        self.assertEqual(
            package_cache.compiler_target({**environment, "CC": "no-such-compiler-nk"}),
            "no-such-compiler-nk:unavailable",
        )
        self.assertEqual(
            package_cache.compiler_target({**environment, "NK_TARGET_TRIPLE": "fixture-target"}),
            "fixture-target",
        )

    def test_relative_cc_resolves_identity_and_target_to_the_same_compiler(self) -> None:
        # A relative CC that PATH does not provide names a file under the repository root.
        # The identity and the target must resolve that one file: otherwise the key could
        # fingerprint one compiler while naming the target of another.
        with tempfile.TemporaryDirectory() as tmp:
            repository = Path(tmp).resolve()
            compiler = repository / "nk-fixture-cc"
            compiler.write_bytes(b"fixture compiler bytes")
            environment = {"CC": "nk-fixture-cc", "PATH": ""}
            completed = subprocess.CompletedProcess(
                [str(compiler), "-dumpmachine"], 0, stdout="fixture-target\n", stderr="",
            )
            with mock.patch.object(package_cache.subprocess, "run",
                                   return_value=completed) as run:
                target = package_cache.compiler_target(
                    environment, repository_root=repository,
                )
            self.assertEqual(target, "fixture-target")
            self.assertEqual(run.call_args.args[0], [str(compiler), "-dumpmachine"])
            self.assertEqual(
                package_cache.compiler_identity(
                    environment=environment, repository_root=repository,
                ),
                f"nk-fixture-cc:{package_cache.sha256_file(compiler)}",
            )
            # Without the repository root neither can find the compiler, and both say so.
            self.assertEqual(
                package_cache.compiler_target(environment),
                "nk-fixture-cc:unavailable",
            )
            self.assertEqual(
                package_cache.compiler_identity(environment=environment),
                "nk-fixture-cc:unavailable",
            )

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
class BoundedJsonArtifactTests(unittest.TestCase):
    """Externally supplied package/cache JSON must fail closed and bounded.

    Every malformed case targets a production entry point and expects the
    named controlled error (BoundedJsonError, or the entry point's own named
    failure that embeds it) -- never a parser-implementation exception.
    """

    def _pkg(self, tmp: Path, name: str, payload: str) -> Path:
        package_dir = Path(tmp) / name
        package_dir.mkdir(parents=True)
        (package_dir / "package.json").write_text(payload, encoding="utf-8")
        return package_dir

    def _deep_text(self, depth: int, payload: str = '1') -> str:
        return '{"format": "nakagawa-aot-package", "n": ' + '{"n":' * depth + payload + '}' * depth + '}'

    def test_object_separators_are_diagnosed_as_separators(self) -> None:
        with self.assertRaisesRegex(package_cache.BoundedJsonError, "comma separators"):
            package_cache.bounded_json_loads(
                '{"a":{"b":1,"c":2},"d":3}', max_members=10, max_items=1,
            )

    def test_nonfinite_numbers_fail_on_the_production_reader(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "numbers.json"
            for number in ("NaN", "Infinity", "-Infinity", "1e400", "-1e400"):
                with self.subTest(number=number):
                    path.write_text('{"value":' + number + '}', encoding="utf-8")
                    with self.assertRaisesRegex(package_cache.BoundedJsonError, "finite"):
                        package_cache.read_bounded_json(path)

    def test_oversized_integer_has_a_named_bounded_diagnosis(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "integer.json"
            path.write_text('{"value":' + "9" * 4301 + '}', encoding="utf-8")
            with self.assertRaisesRegex(package_cache.BoundedJsonError, "integer.*digit") as caught:
                package_cache.read_bounded_json(path)
            self.assertLess(len(str(caught.exception)), 256)

    def test_integer_ceiling_is_independent_of_python_global_settings(self) -> None:
        # Disabling Python's optional global guard must not unbound this reader.
        with mock.patch.object(package_cache, "int", return_value=1, create=True) as conversion:
            with self.assertRaisesRegex(package_cache.BoundedJsonError, "integer.*digit"):
                package_cache.bounded_json_loads("9" * 4301)
            conversion.assert_not_called()

    def test_supported_numeric_values_preserve_their_types(self) -> None:
        result = package_cache.bounded_json_loads('[0,-12,1.25,1e2,1e-2]')
        self.assertEqual(result, [0, -12, 1.25, 100.0, 0.01])
        self.assertEqual([type(item) for item in result], [int, int, float, float, float])

    def test_integer_at_digit_ceiling_is_accepted_with_either_sign(self) -> None:
        digits = "9" * package_cache.MAX_CACHE_JSON_INTEGER_DIGITS
        for sign in ("", "-"):
            with self.subTest(sign=sign):
                self.assertEqual(package_cache.bounded_json_loads(sign + digits), int(sign + digits))

    def test_valid_package_at_exact_byte_limit_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            package_dir = Path(tmp) / "at_limit"
            package_dir.mkdir()
            base = '{"format": "nakagawa-aot-package", "schema_version": 2, "pad": "'
            text = base + "A" * (package_cache.MAX_CACHE_JSON_BYTES - len(base) - 2) + '"}'
            self.assertEqual(len(text.encode("utf-8")), package_cache.MAX_CACHE_JSON_BYTES)
            (package_dir / "package.json").write_text(text, encoding="utf-8")
            (package_dir / "build-report.json").write_text("{}", encoding="utf-8")
            # Valid JSON at the exact ceiling parses; validation fails on contract
            # fields only, with no byte-limit or parse complaint.
            valid, reason = package_cache.validate_package_cache(package_dir)
            self.assertFalse(valid)
            self.assertNotIn("limit", reason)
            self.assertNotIn("unreadable", reason)

    def test_one_byte_over_limit_is_rejected_with_named_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            prefix = '{"format": "nakagawa-aot-package", "pad": "'
            pad = package_cache.MAX_CACHE_JSON_BYTES + 1 - len(prefix) - 2
            package_dir = self._pkg(tmp, "over", prefix + "A" * pad + '"}')
            size = (package_dir / "package.json").stat().st_size
            self.assertEqual(size, package_cache.MAX_CACHE_JSON_BYTES + 1)
            valid, reason = package_cache.validate_package_cache(package_dir)
            self.assertFalse(valid)
            self.assertIn("byte", reason)

    def test_growing_file_during_read_cannot_bypass_the_ceiling(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "growing.json"
            path.write_text("{}", encoding="utf-8")
            limit = package_cache.MAX_CACHE_JSON_BYTES

            def growing_read(fd: int, amount: int) -> bytes:
                # Simulate the file growing past the ceiling while the handle
                # is open, after fstat saw the small size: the kernel returns
                # more data than fstat accounted for.
                return b"x" * (limit + 1)

            with mock.patch.object(package_cache.os, "read", side_effect=growing_read):
                with self.assertRaisesRegex(package_cache.BoundedJsonError, "grew past"):
                    package_cache.read_bounded_json(path)

    def test_deep_package_json_fails_closed_with_named_error(self) -> None:
        # 100k levels stays under the byte ceiling so the depth gate is the
        # one that fires.
        with tempfile.TemporaryDirectory() as tmp:
            package_dir = self._pkg(tmp, "deep", self._deep_text(100_000))
            valid, reason = package_cache.validate_package_cache(package_dir)
            self.assertFalse(valid)
            self.assertIn("nesting", reason)

    def test_read_bounded_json_depth_and_counts_have_named_errors(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "deep.json"
            path.write_text(self._deep_text(33), encoding="utf-8")
            with self.assertRaisesRegex(package_cache.BoundedJsonError, "nesting exceeds 32"):
                package_cache.read_bounded_json(path)
            path.write_text(
                '{"a":' + '[' * 33 + ']' * 33 + '}', encoding="utf-8"
            )
            with self.assertRaisesRegex(package_cache.BoundedJsonError, "nesting exceeds 32"):
                package_cache.read_bounded_json(path)

    def test_excessive_member_count_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "members.json"
            members = ",".join(f'"k{i}": 1' for i in range(package_cache.MAX_CACHE_JSON_MEMBERS + 1))
            path.write_text("{" + members + "}", encoding="utf-8")
            with self.assertRaisesRegex(package_cache.BoundedJsonError, "object members"):
                package_cache.read_bounded_json(path)

    def test_excessive_node_count_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "nodes.json"
            items = ",".join("1" for _ in range(package_cache.MAX_CACHE_JSON_NODES + 1))
            path.write_text("[" + items + "]", encoding="utf-8")
            with self.assertRaisesRegex(package_cache.BoundedJsonError, "structural nodes"):
                package_cache.read_bounded_json(path)

    def test_build_report_reader_has_its_own_named_node_ceiling(self) -> None:
        # The report reads under its own ceilings, so the node limit that binds
        # it is 262144 rather than the shared 65536, and it stays named.
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "build-report.json"
            shared = ",".join("1" for _ in range(package_cache.MAX_CACHE_JSON_NODES + 1))
            path.write_text("[" + shared + "]", encoding="utf-8")
            # Under the shared node ceiling, so the report route accepts it.
            package_cache.read_build_report_json(path)
            with self.assertRaisesRegex(package_cache.BoundedJsonError, "structural nodes"):
                package_cache.read_bounded_json(path)
            report = ",".join("1" for _ in range(package_cache.MAX_BUILD_REPORT_JSON_NODES + 1))
            path.write_text("[" + report + "]", encoding="utf-8")
            with self.assertRaisesRegex(
                package_cache.BoundedJsonError, "262144 structural nodes"
            ):
                package_cache.read_build_report_json(path)

    def test_duplicate_keys_are_rejected_with_named_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "dup.json"
            path.write_text('{"format": "a", "format": "b"}', encoding="utf-8")
            with self.assertRaisesRegex(package_cache.BoundedJsonError, "duplicate JSON field: format"):
                package_cache.read_bounded_json(path)

    def test_malformed_utf8_is_a_named_controlled_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad_utf8.json"
            path.write_bytes(b'{"format": "\xff\xfe"}')
            with self.assertRaisesRegex(package_cache.BoundedJsonError, "not valid UTF-8"):
                package_cache.read_bounded_json(path)

    def test_invalid_json_is_a_named_controlled_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "invalid.json"
            path.write_text("{not json", encoding="utf-8")
            with self.assertRaisesRegex(package_cache.BoundedJsonError, "not valid JSON"):
                package_cache.read_bounded_json(path)

    def test_wrong_top_level_type_fails_the_cache_contract(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            package_dir = self._pkg(tmp, "wrong_top", "[1, 2, 3]")
            (package_dir / "build-report.json").write_text("{}", encoding="utf-8")
            valid, reason = package_cache.validate_package_cache(package_dir)
            self.assertFalse(valid)
            self.assertIn("JSON objects", reason)

    def test_oversize_completion_manifest_fails_validation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            package_dir = Path(tmp) / "big_completion"
            package_dir.mkdir()
            (package_dir / "completion-manifest.json").write_text(
                '{"format": "x", "pad": "' + "A" * package_cache.MAX_CACHE_JSON_BYTES + '"}',
                encoding="utf-8",
            )
            valid, reason, _ = package_cache.validate_completion_manifest(package_dir)
            self.assertFalse(valid)
            self.assertIn("unreadable", reason)

    def test_deep_completion_manifest_fails_validation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            package_dir = Path(tmp) / "deep_completion"
            package_dir.mkdir()
            (package_dir / "completion-manifest.json").write_text(
                self._deep_text(100_000), encoding="utf-8"
            )
            valid, reason, _ = package_cache.validate_completion_manifest(package_dir)
            self.assertFalse(valid)
            self.assertIn("unreadable", reason)

    def test_duplicate_key_diagnosis_does_not_echo_attacker_bulk(self) -> None:
        """Rejection stays named, but a hostile key is not quoted back.

        Bounding the artifact does not bound the message it produces. A
        duplicate key under the byte ceiling is attacker-chosen text that would
        otherwise land verbatim in an exception and in whatever log captures
        it, so only a short prefix is echoed. The rejection itself is
        unchanged: still a named BoundedJsonError, not a silent last-wins.
        """
        bulk = "A" * 400_000
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "dup.json"
            # Two copies of the bulk key stay under MAX_CACHE_JSON_BYTES, so
            # this is a duplicate-key rejection, not a byte-ceiling rejection.
            text = '{"%s": 1, "%s": 2}' % (bulk, bulk)
            self.assertLess(len(text.encode("utf-8")), package_cache.MAX_CACHE_JSON_BYTES)
            path.write_text(text, encoding="utf-8")
            with self.assertRaises(package_cache.BoundedJsonError) as caught:
                package_cache.read_bounded_json(path)
            message = str(caught.exception)
            self.assertIn("duplicate JSON field", message)
            self.assertIn("...[truncated]", message)
            self.assertLess(len(message), 256)
            self.assertNotIn(bulk, message)

    def test_node_counter_never_undercounts_any_scalar_or_trailing_string(self) -> None:
        """The structural node count is a true lower bound, not a leaky one.

        Two ways the counter used to lose a node: a document whose final node
        was a string closer finished past the ceiling, because the in-string
        branch continues before the per-character test and there was no
        post-loop test; and the "was the previous character numeric" seed was
        the empty string, which is a substring of every string, so a document
        that *starts* with a digit had its leading number skipped entirely.
        """
        one_node = ["1", "-2", "1.5", "1e5", "-1.5e-3", "true", "false",
                    "null", '"s"', '""', "{}", "[]"]
        for text in one_node:
            with self.subTest(text=text):
                # Zero nodes is below any document, so it must be rejected.
                with self.assertRaises(package_cache.BoundedJsonError):
                    package_cache._bounded_json_scan(
                        text, max_depth=32, max_members=1 << 20,
                        max_items=1 << 20, max_nodes=0,
                    )
                # One node is exactly the document, so it must be accepted.
                package_cache._bounded_json_scan(
                    text, max_depth=32, max_members=1 << 20,
                    max_items=1 << 20, max_nodes=1,
                )
        # A document whose last node is a string closer: 2 nodes, not 1.
        with self.assertRaises(package_cache.BoundedJsonError):
            package_cache._bounded_json_scan(
                '["a"]', max_depth=32, max_members=1 << 20,
                max_items=1 << 20, max_nodes=1,
            )
        package_cache._bounded_json_scan(
            '["a"]', max_depth=32, max_members=1 << 20,
            max_items=1 << 20, max_nodes=2,
        )

    def test_read_loop_is_bounded_even_if_a_read_returns_more_than_requested(self) -> None:
        """The reader must terminate and fail closed, not loop.

        A real read never returns more than it was asked for, but the loop
        condition is written so that an over-long read short-circuits instead
        of driving the counter negative and re-entering os.read.
        """
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "short.json"
            path.write_text('{"a": 1}', encoding="utf-8")
            real_read = os.read

            def overlong_read(fd: int, n: int) -> bytes:
                return real_read(fd, n) + b"A" * (package_cache.MAX_CACHE_JSON_BYTES + 1)

            with mock.patch.object(package_cache.os, "read", side_effect=overlong_read):
                with self.assertRaises(package_cache.BoundedJsonError) as caught:
                    package_cache.read_bounded_json(path)
            self.assertIn("grew past", str(caught.exception))

    def test_bounded_echo_truncates_strings_and_bounds_field_samples(self) -> None:
        """Untrusted text is quoted as a prefix, untrusted lists as a sample."""
        self.assertEqual(package_cache.bounded_echo("short"), "short")
        self.assertEqual(
            package_cache.bounded_echo("A" * 200),
            "A" * package_cache.MAX_JSON_ECHO_CHARS + "...[truncated]",
        )
        # Non-strings are rendered but still truncated, so a huge nested value
        # cannot be quoted whole either.
        self.assertEqual(package_cache.bounded_echo(7), "7")
        self.assertLess(len(package_cache.bounded_echo(["B" * 5000])), 256)
        many = package_cache.bounded_echo_fields({"f%03d" % i for i in range(500)})
        self.assertIn("(+492 more)", many)
        self.assertLess(len(many), 8 * (package_cache.MAX_JSON_ECHO_CHARS + 2) + 64)


if __name__ == "__main__":
    unittest.main()
