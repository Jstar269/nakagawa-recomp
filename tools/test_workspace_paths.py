# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Workspace path handling with spaces, Unicode, and long paths (#368).

Exercises the repository's path-handling helpers against hostile-but-legal
workspace shapes: the workspace Doctor's long-path and directory probes, the
package route's Make-safety checks, the XB extractor's containment utilities,
and the clean preview's own planning. All fixtures live in temporary
directories; every helper must either handle the path correctly or refuse it
with a clear, named error -- never raise a bare OSError/ValueError.
"""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import extract_xb  # noqa: E402
import nk_clean  # noqa: E402
import nk_doctor  # noqa: E402
import nk_doctor_checks  # noqa: E402
import nk_doctor_core  # noqa: E402
import title_codegen_plan  # noqa: E402


def _write(path: Path, data: bytes = b"x") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def _make_workspace(base: Path) -> Path:
    root = base / "ws"
    for marker in nk_clean.WORKSPACE_ANCHORS:
        _write(root / marker)
    return root


class PackageRouteMakeSafetyTests(unittest.TestCase):
    def test_space_in_path_is_refused_with_actionable_message(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            spaced = Path(tmp) / "out dir" / "package"
            with self.assertRaises(title_codegen_plan.PackageRouteError) as ctx:
                title_codegen_plan._absolute_path(spaced, "output-dir")
            self.assertEqual(ctx.exception.code, "PACKAGE_UNSUPPORTED_PATH")
            self.assertIn("in the works", str(ctx.exception))

    def test_unicode_without_space_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            unicode_path = Path(tmp) / "ビルド出力" / "package"
            resolved = title_codegen_plan._absolute_path(unicode_path, "output-dir")
            self.assertIsInstance(resolved, Path)
            self.assertEqual(resolved.name, "package")

    def test_fullwidth_space_is_refused_as_unmakeable(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "出力\u3000dir"
            with self.assertRaises(title_codegen_plan.PackageRouteError) as ctx:
                title_codegen_plan._absolute_path(path, "output-dir")
            self.assertEqual(ctx.exception.code, "PACKAGE_UNSUPPORTED_PATH")

    def test_control_character_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad\x01name"
            with self.assertRaises(title_codegen_plan.PackageRouteError) as ctx:
                title_codegen_plan._absolute_path(path, "output-dir")
            self.assertEqual(ctx.exception.code, "PACKAGE_INVALID_PATH")

    def test_long_path_is_handled_or_clearly_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            long_path = Path(tmp) / ("L" * 180) / ("M" * 180) / "package"
            try:
                resolved = title_codegen_plan._absolute_path(long_path, "output-dir")
            except title_codegen_plan.PackageRouteError as exc:
                self.assertIn(exc.code, {"PACKAGE_INVALID_PATH", "PACKAGE_UNSUPPORTED_PATH"})
                self.assertTrue(str(exc))
            else:
                self.assertIsInstance(resolved, Path)

    def test_make_safe_build_root_env_override_needs_make_safe_value(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            safe_root = base / "safe_root"
            safe_root.mkdir()
            safe_str = str(safe_root)
            if any(
                char.isspace() or char in title_codegen_plan._MAKE_UNSAFE_PATH_CHARS
                for char in safe_str
            ):
                self.skipTest("temporary directory is not Make-safe on this host")
            with mock.patch.dict(os.environ, {"NK_BUILD_ROOT": safe_str}):
                resolved = title_codegen_plan._resolve_make_safe_build_root(
                    base / "any" / "dest"
                )
            self.assertEqual(resolved, safe_root.resolve())

            spaced_env = str(base / "bad root")
            with mock.patch.dict(os.environ, {"NK_BUILD_ROOT": spaced_env}):
                with mock.patch(
                    "title_codegen_plan._windows_short_path", return_value=None
                ):
                    with self.assertRaises(title_codegen_plan.PackageRouteError) as ctx:
                        title_codegen_plan._resolve_make_safe_build_root(
                            base / "dest with space" / "out"
                        )
            self.assertEqual(ctx.exception.code, "PACKAGE_UNSUPPORTED_PATH")
            self.assertIn("set NK_BUILD_ROOT to a folder without spaces", str(ctx.exception))


class DoctorWorkspacePathTests(unittest.TestCase):
    def test_long_paths_measures_space_and_unicode_deepest_path(self) -> None:
        root = Path(tempfile.mkdtemp(prefix="nakagawa 空白 "))
        self.addCleanup(lambda: os.path.isdir(root) and __import__("shutil").rmtree(root, ignore_errors=True))
        deepest = (
            Path("build") / ("テスト" * 20) / ("x" * 180) / "final file.bin"
        )
        report = nk_doctor.Report(root, "build")
        with mock.patch.object(
            nk_doctor_checks, "query_windows_long_paths_enabled", return_value=False
        ), mock.patch.object(nk_doctor_checks.platform, "system", return_value="Windows"):
            nk_doctor_checks.check_long_paths(report, root, deepest_rel=deepest)

        results = [r for r in report.results if r.code == "LONG_PATHS"]
        self.assertEqual(len(results), 1)
        expected = root.resolve() / deepest
        self.assertEqual(results[0].metadata.get("total_len"), len(str(expected)))
        self.assertGreater(results[0].metadata.get("total_len", 0), 260)
        self.assertTrue(results[0].metadata.get("exceeds_260"))
        self.assertEqual(results[0].status, "WARN")
        self.assertIn("テスト", results[0].metadata.get("deepest_path", ""))
        self.assertIn("final file.bin", results[0].metadata.get("deepest_path", ""))

    def test_bounded_directory_probe_handles_unicode_and_space_names(self) -> None:
        with tempfile.TemporaryDirectory(prefix="doktor ünicode ") as tmp:
            base = Path(tmp) / "data データ"
            _write(base / "plain.txt")
            _write(base / "üñî ファイル.txt")
            _write(base / "nested dir" / "日本語.log")
            count, error = nk_doctor_core._bounded_nonempty_directory(base)
            self.assertIsNone(error)
            self.assertEqual(count, 3)


class ExtractXbPathUtilityTests(unittest.TestCase):
    def test_member_path_accepts_unicode_with_inner_space(self) -> None:
        name = "data/メニュー コモン/x.bin"
        self.assertEqual(extract_xb.normalize_member_path(name), name)

    def test_member_path_refuses_traversal_and_overlong_component(self) -> None:
        with self.assertRaises(extract_xb.UnsafeArchivePathError):
            extract_xb.normalize_member_path("../escape.bin")
        with self.assertRaises(extract_xb.UnsafeArchivePathError):
            extract_xb.normalize_member_path("data/" + "A" * (extract_xb.MAX_MEMBER_COMPONENT_CHARS + 1))

    def test_contained_path_resolves_unicode_and_refuses_escape(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root_real = extract_xb.canonical_root(tmp)
            target = extract_xb.contained_path(
                root_real, "ディレクトリ/file with space.bin"
            )
            self.assertEqual(
                target,
                os.path.join(root_real, "ディレクトリ", "file with space.bin"),
            )
            with self.assertRaises(extract_xb.UnsafeArchivePathError):
                extract_xb.contained_path(root_real, "../escape.bin")


class CleanWorkspacePathTests(unittest.TestCase):
    def test_plan_and_delete_under_space_and_unicode_root(self) -> None:
        root = Path(tempfile.mkdtemp(prefix="nk clean 試験 "))
        for marker in nk_clean.WORKSPACE_ANCHORS:
            _write(root / marker)
        doomed = _write(root / "build" / "サブ ディレクトリ" / "オブジェクト.o", b"obj")
        keep = _write(root / "keys" / "secret.bin", b"key")

        preview = nk_clean.plan_clean(root, tracked=frozenset(), include_title_outputs=True)
        self.assertFalse(preview.errors, preview.errors)
        selected = {f.rel for f in preview.files}
        self.assertIn("build/サブ ディレクトリ/オブジェクト.o", selected)
        text = nk_clean.render(preview, confirm=True)
        self.assertIn("オブジェクト.o", text)

        failures = nk_clean.execute(preview)
        self.assertEqual(failures, [])
        self.assertFalse(doomed.exists())
        self.assertTrue(keep.exists())

    def test_long_path_under_build_is_selected_or_clearly_refused(self) -> None:
        root = Path(tempfile.mkdtemp(prefix="nk clean long "))
        for marker in nk_clean.WORKSPACE_ANCHORS:
            _write(root / marker)
        shallow = _write(root / "build" / "shallow.o", b"obj")
        deep_parent = root / "build" / ("x" * 100) / ("y" * 100) / ("z" * 100)
        deep_file = deep_parent / "long file.bin"
        created = False
        try:
            deep_parent.mkdir(parents=True, exist_ok=True)
            deep_file.write_bytes(b"deep")
            created = len(str(deep_file)) > 260
        except OSError:
            created = False

        preview = nk_clean.plan_clean(root, tracked=frozenset(), include_title_outputs=True)
        selected = {f.rel for f in preview.files}
        scan_errors = [message for _, message in preview.errors]
        if created:
            long_rel = deep_file.relative_to(root).as_posix()
            self.assertTrue(
                long_rel in selected or scan_errors,
                "a >260-char path must be either selected or reported as a scan refusal",
            )
        else:
            self.assertTrue(
                preview.files,
                f"shallow tree must still be previewed (errors={scan_errors})",
            )
        self.assertTrue(shallow.exists())
        render = nk_clean.render(preview, confirm=False)
        self.assertIn("DRY-RUN", render)

    def test_containment_refuses_long_outside_component(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            root = _make_workspace(base)
            root_real = os.path.realpath(os.path.abspath(root))
            outside = _write(base / ("o" * 200) / ("p" * 200) / "file.bin", b"out")
            with self.assertRaises(nk_clean.CleanRefusedError):
                nk_clean._require_within(root_real, outside)


if __name__ == "__main__":
    unittest.main()
