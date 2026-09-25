# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Workspace path handling with spaces, Unicode, and long paths (#368).

Exercises the repository's path-handling helpers against hostile-but-legal
workspace shapes: the workspace Doctor's long-path and directory probes, the
package route's Make-safety checks, the XB extractor's containment utilities,
and the clean preview's own planning. All fixtures live in temporary
directories; every helper must either handle the path correctly or refuse it
with a clear, named error -- never raise a bare OSError/ValueError.

The tracked-tree layer additionally enforces the #368 acceptance item that every
first-party absolute machine/workspace path is removed, generated locally, an
explicit test/example, or a documented platform convention.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
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
import publish_audit  # noqa: E402
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


class TrackedMachinePathTests(unittest.TestCase):
    """No undeclared absolute machine/workspace path in tracked first-party files (#368).

    Two structural layers run over ``git ls-files`` with the third-party imports
    excluded:

    * a user-profile or workspace root never qualifies, in any tracked file
      (tests included), outside an explicit exception;
    * every other drive-rooted token must sit on a documented platform
      convention or on an explicit fixture/prose exception below.

    Files that are tests or examples by construction (``tools/test_*``,
    ``tests/``, ``fixtures/``, ``*.test.*``) are the explicit test/example class
    of the acceptance item; the first layer still applies to them, and the
    inventory of their fixture paths lives in the issue rather than here.
    """

    THIRD_PARTY_PREFIXES = ("src/rt/atrac3p/", "font/")

    #: Files allowed to name a user-profile/workspace root, with the reason the
    #: path is first-party-acceptable.
    ROOT_PATH_EXCEPTIONS = {
        "assets/public_source_profile.json": (
            "publication policy declares the maintainer's private workspace root; "
            "policy files are maintainer-owned and never edited by agents"
        ),
    }

    USER_OR_WORKSPACE_ROOTS = (
        (
            re.compile(r"[A-Za-z]:[\\/]{1,2}Users(?:[\\/]|(?![A-Za-z0-9_]))", re.IGNORECASE),
            "Windows user-profile path",
        ),
        (
            re.compile(r"[A-Za-z]:[\\/]{1,2}nk(?![A-Za-z0-9_])", re.IGNORECASE),
            "Nakagawa workspace root",
        ),
        (re.compile(r"/(?:home|Users)/[^\s/]+"), "POSIX user-home path"),
        (re.compile(r"/mnt/[a-z]/(?:Users|nk)(?:[\\/]|(?![A-Za-z0-9_]))"), "WSL user/workspace root"),
    )

    DRIVE_TOKEN = re.compile(r"(?<![A-Za-z0-9_])[A-Za-z]:[\\/][A-Za-z0-9_./\\-]*")

    #: Documented platform conventions and example placeholders, by normalised
    #: lower-case prefix. Every reason naming docs/SETUP.md must stay true there.
    ALLOWED_DRIVE_PREFIXES = {
        "c:/msys64": "default MSYS2 UCRT64 install root (docs/SETUP.md)",
        "c:\\msys64": "default MSYS2 UCRT64 install root (docs/SETUP.md)",
        "c:/vulkansdk": "default Vulkan SDK root (docs/SETUP.md)",
        "c:\\vulkansdk": "default Vulkan SDK root (docs/SETUP.md)",
        "c:/windows": "Windows system directory, the WINDIR fallback (docs/SETUP.md)",
        "c:\\windows": "Windows system directory, the WINDIR fallback (docs/SETUP.md)",
        "c:/path/": "placeholder root in usage examples",
        "c:\\path\\": "placeholder root in usage examples",
    }

    #: Exact normalised tokens that name no machine location, by reason.
    ALLOWED_DRIVE_TOKENS = {
        "c:\\": "bare drive root named by detectors and comment examples",
        "c:/x": "hostile drive-rooted input in the VFS containment selftest (src/rt/vfs_selftest.c)",
        "c:\\foo": "hostile device-path input in the VFS containment selftest (src/rt/vfs_selftest.c)",
        "c:\\extracted": "SR_DATAROOT comment example, an absolute data root (src/rt/hle.c)",
    }

    @staticmethod
    def _collapse_backslashes(token: str) -> str:
        while "\\\\" in token:
            token = token.replace("\\\\", "\\")
        return token

    @staticmethod
    def _is_explicit_test_or_fixture(rel: str) -> bool:
        parts = rel.split("/")
        name = parts[-1]
        if any(part in parts for part in ("tests", "fixtures")):
            return True
        if name.startswith("test_") or name.endswith(("_test.py", "_test.ps1")):
            return True
        if re.search(r"\.test\.(?:ts|tsx|js|mjs|cjs)$", name) or ".spec." in name:
            return True
        return False

    def _tracked_texts(self) -> list[tuple[str, str]]:
        if shutil.which("git") is None:
            self.skipTest("git is required to enumerate tracked first-party files")
        try:
            listing = subprocess.run(
                ["git", "-C", str(ROOT), "ls-files", "-z"],
                capture_output=True,
                text=True,
                check=True,
            ).stdout
        except (OSError, subprocess.CalledProcessError) as exc:
            self.fail(f"git ls-files failed to enumerate tracked files: {exc}")
        texts: list[tuple[str, str]] = []
        for rel in (entry for entry in listing.split("\0") if entry):
            if rel.startswith(self.THIRD_PARTY_PREFIXES):
                continue
            path = ROOT / rel
            try:
                data = path.read_bytes()
            except OSError:
                continue
            if len(data) > 8 * 1024 * 1024 or publish_audit._is_binary_bytes(data):
                continue
            text = publish_audit._decode_text_safe(data)
            if text is None:
                continue
            texts.append((rel, text))
        return texts

    @staticmethod
    def _line_of(text: str, offset: int) -> int:
        return text.count("\n", 0, offset) + 1

    def test_tracked_files_name_no_user_profile_or_workspace_root(self) -> None:
        violations: list[str] = []
        for rel, text in self._tracked_texts():
            if rel in self.ROOT_PATH_EXCEPTIONS:
                continue
            for pattern, kind in self.USER_OR_WORKSPACE_ROOTS:
                for match in pattern.finditer(text):
                    violations.append(
                        f"{rel}:{self._line_of(text, match.start())}: {kind}: {match.group(0)!r}"
                    )
        self.assertEqual(
            violations,
            [],
            "tracked first-party files must not name a user-profile or workspace "
            "root; add the file and its reason to ROOT_PATH_EXCEPTIONS only for a "
            "deliberate policy declaration (#368):\n  " + "\n  ".join(violations),
        )

    def test_drive_rooted_tokens_are_documented_or_explicit(self) -> None:
        violations: list[str] = []
        for rel, text in self._tracked_texts():
            if rel in self.ROOT_PATH_EXCEPTIONS or self._is_explicit_test_or_fixture(rel):
                continue
            for match in self.DRIVE_TOKEN.finditer(text):
                token = self._collapse_backslashes(match.group(0)).lower()
                if token in self.ALLOWED_DRIVE_TOKENS:
                    continue
                if any(token.startswith(prefix) for prefix in self.ALLOWED_DRIVE_PREFIXES):
                    continue
                violations.append(
                    f"{rel}:{self._line_of(text, match.start())}: {match.group(0)!r}"
                )
        self.assertEqual(
            violations,
            [],
            "every drive-rooted token in first-party non-test code needs a "
            "documented platform convention or an explicit exception with a "
            "reason (#368):\n  " + "\n  ".join(violations),
        )

    def test_platform_conventions_are_documented_in_setup(self) -> None:
        setup = (ROOT / "docs" / "SETUP.md").read_text(encoding="utf-8")
        for literal in ("C:\\msys64\\ucrt64\\bin", "C:\\VulkanSDK", "C:\\Windows"):
            self.assertIn(
                literal,
                setup,
                f"docs/SETUP.md must document the {literal!r} platform convention "
                "that ALLOWED_DRIVE_PREFIXES defers to (#368)",
            )


if __name__ == "__main__":
    unittest.main()
