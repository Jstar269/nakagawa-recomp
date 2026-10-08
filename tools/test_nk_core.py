# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Unit tests for the portable nk_core preparation & runtime library."""

from __future__ import annotations

import ast
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))

import nk_cli  # noqa: E402
import title_manifest  # noqa: E402
from nk_core import package_cache  # noqa: E402
from nk_core import (
    CancellationToken,
    GameLibrary,
    IsoInspectionError,
    LibraryGameRecord,
    PreparationEngine,
    ProgressEvent,
    RuntimeLauncher,
    RuntimeLaunchError,
    TitleProfile,
    TitleRegistry,
    inspect_iso,
)

_ISSUE_REFERENCE_RE = re.compile(r"#[0-9]+")
_C_TOKEN_RE = re.compile(
    r'//[^\r\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"', re.DOTALL
)


def _python_non_docstring_literals(path: Path) -> list[tuple[int, str]]:
    source = path.read_text(encoding="utf-8")
    tree = ast.parse(source, filename=str(path))
    docstring_positions: set[tuple[int, int]] = set()
    for node in ast.walk(tree):
        if not isinstance(
            node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)
        ) or not node.body:
            continue
        first = node.body[0]
        if (
            isinstance(first, ast.Expr)
            and isinstance(first.value, ast.Constant)
            and isinstance(first.value.value, str)
        ):
            docstring_positions.add((first.value.lineno, first.value.col_offset))
    return [
        (node.lineno, node.value)
        for node in ast.walk(tree)
        if isinstance(node, ast.Constant)
        and isinstance(node.value, str)
        and (node.lineno, node.col_offset) not in docstring_positions
    ]


def _build_param_sfo(disc_id: str, title: str = "Synthetic Test Title",
                     version: str = "1.00") -> bytes:
    """Build a real PSP PARAM.SFO structure.

    The fixture used to embed the bare disc-id string and rely on the inspector
    scanning raw image bytes for it. That path is a guess, not evidence, and no
    longer satisfies the registry -- so a fixture that wants a *supported* disc
    has to present the structure a real disc presents.
    """
    entries = [
        ("DISC_ID", 0x0204, disc_id.encode("utf-8") + bytes([0])),
        ("DISC_VERSION", 0x0204, version.encode("utf-8") + bytes([0])),
        ("TITLE", 0x0204, title.encode("utf-8") + bytes([0])),
    ]
    entries.sort(key=lambda e: e[0])

    key_table = bytearray()
    data_table = bytearray()
    entry_table = bytearray()
    for key, fmt, val in entries:
        k_off = len(key_table)
        key_table.extend(key.encode("utf-8") + bytes([0]))
        d_off = len(data_table)
        d_len = len(val)
        data_table.extend(val)
        while len(data_table) % 4 != 0:
            data_table.append(0)
        entry_table.extend(struct.pack("<HHIII", k_off, fmt, d_len, d_len, d_off))

    key_table_start = 20 + len(entry_table)
    data_table_start = key_table_start + len(key_table)
    while data_table_start % 4 != 0:
        key_table.append(0)
        data_table_start += 1
    header = struct.pack("<4s4sIII", bytes([0]) + b"PSF", bytes([1, 1, 0, 0]),
                         key_table_start, data_table_start, len(entries))
    return bytes(header + entry_table + key_table + data_table)


def _create_mock_iso(path: Path, disc_id: str = "TEST00001", volume_id: str = "SYNTHETIC_GAME") -> None:
    from test_iso_parity import create_test_iso

    create_test_iso(path, disc_id=disc_id, volume_id=volume_id)


def _write_title_runtime(
    root: Path, name: str, exe_ext: str = "", image: bool = True
) -> tuple[Path, Path | None]:
    """Create a title's own build product under the shared build/<name> layout.

    This is the only layout generic launch resolution may resolve a runtime
    from: the directory and file names come from the selected title's
    validated build identity (see tools/nk_core/launcher.py NAME_SOURCES).
    """
    build_dir = root / "build" / name
    build_dir.mkdir(parents=True, exist_ok=True)
    exe = build_dir / f"{name}{exe_ext}"
    exe.write_bytes(b"MZfake" if exe_ext else b"ELFfake")
    img: Path | None = None
    if image:
        img = build_dir / f"{name}_image.bin"
        img.write_bytes(b"image")
    return exe, img


def _write_stale_other_title_artifacts(root: Path) -> None:
    """Plant artifacts of a different (retail-shaped) title in the workspace.

    These are source-owned synthetic stand-ins for the exact paths the legacy
    generic candidates used to probe: a stale sibling-title build must be
    irrelevant to every other title's launch resolution (#366). No real retail
    byte is used or needed here.
    """
    build_dir = root / "build" / "hst"
    build_dir.mkdir(parents=True, exist_ok=True)
    (build_dir / "hst").write_bytes(b"ELFfake")
    (build_dir / "hst.exe").write_bytes(b"MZfake")
    (build_dir / "hst_image.bin").write_bytes(b"image")
    (root / "hst").write_bytes(b"ELFfake")
    (root / "hst.exe").write_bytes(b"MZfake")
    (root / "hst_image.bin").write_bytes(b"image")
    runtime_dir = root / "runtime"
    runtime_dir.mkdir(parents=True, exist_ok=True)
    (runtime_dir / "hst_image.bin").write_bytes(b"image")


class NkCoreTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = Path(tempfile.mkdtemp(prefix="test_nk_core_"))

    def tearDown(self) -> None:
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_user_facing_text_does_not_cite_issue_numbers(self) -> None:
        repo_root = Path(__file__).resolve().parents[1]
        failures: list[str] = []
        c_paths = [
            *sorted((repo_root / "src" / "player").glob("*.c")),
            repo_root / "src" / "core" / "nk_font.c",
            repo_root / "src" / "core" / "nk_launch.c",
            repo_root / "src" / "core" / "nk_title_manifest.c",
        ]
        for path in c_paths:
            source = path.read_text(encoding="utf-8")
            for match in _C_TOKEN_RE.finditer(source):
                token = match.group()
                if token.startswith('"') and _ISSUE_REFERENCE_RE.search(token):
                    line = source.count("\n", 0, match.start()) + 1
                    failures.append(f"{path.relative_to(repo_root)}:{line}: {token}")

        python_paths = [
            repo_root / "tools" / "nk_cli.py",
            repo_root / "tools" / "nk_doctor_checks.py",
            repo_root / "tools" / "title_manifest.py",
            *sorted((repo_root / "tools" / "nk_core").glob("*.py")),
        ]
        for path in python_paths:
            for line, literal in _python_non_docstring_literals(path):
                if _ISSUE_REFERENCE_RE.search(literal):
                    failures.append(
                        f"{path.relative_to(repo_root)}:{line}: {literal!r}"
                    )

        prerequisite_manifest = json.loads(
            (repo_root / "assets" / "prereq_manifest.json").read_text(
                encoding="utf-8"
            )
        )
        for hook in prerequisite_manifest.get("font_source_hooks", []):
            if not isinstance(hook, dict):
                continue
            message = hook.get("failure_message")
            if isinstance(message, str) and _ISSUE_REFERENCE_RE.search(message):
                failures.append(f"assets/prereq_manifest.json: {message!r}")

        renderer = (repo_root / "src" / "player" / "ui_renderer.c").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("draw_issue_links", renderer)
        self.assertNotRegex(renderer, r'"#%[0-9]*[du]"')
        self.assertEqual(failures, [])

        summary = nk_cli._bringup_human_summary(
            {
                "preflight_checks": [],
                "failure_class": "ENTRY_NOT_COMPILED",
                "issue_numbers": [308],
                "reached_stage": "compile",
            }
        )
        self.assertNotRegex(summary, _ISSUE_REFERENCE_RE)

    def test_title_registry_matching_and_normalization(self) -> None:
        reg = TitleRegistry(include_defaults=True)
        # Check standard synthetic title matches with or without hyphens
        p1 = reg.lookup_by_disc_id("TEST00001")
        p2 = reg.lookup_by_disc_id("test-00001")
        p3 = reg.lookup_by_disc_id("  TEST_00001  ")
        self.assertIsNotNone(p1)
        self.assertEqual(p1, p2)
        self.assertEqual(p1, p3)
        self.assertEqual(p1.id, "synthetic-allegrex-v1")

        # Check second synthetic title
        p5 = reg.lookup_by_disc_id("TEST-00005")
        self.assertIsNotNone(p5)
        self.assertEqual(p5.id, "pspdev-phase5-v1")

        # Check retail disc IDs are not in public registry
        self.assertIsNone(reg.lookup_by_disc_id("UCUS98701"))
        self.assertIsNone(reg.lookup_by_disc_id("UCES01402"))

        # Check unsupported disc ID
        none_profile = reg.lookup_by_disc_id("ULUS99999")
        self.assertIsNone(none_profile)

    def test_iso_inspection_success(self) -> None:
        iso_file = self.temp_dir / "test_game.iso"
        _create_mock_iso(iso_file, disc_id="TEST00001", volume_id="SYNTHETIC_TEST")

        meta = inspect_iso(iso_file)
        self.assertEqual(meta.disc_id, "TEST00001")
        self.assertTrue(meta.is_supported)
        self.assertIsNotNone(meta.matched_profile)
        self.assertEqual(meta.matched_profile.id, "synthetic-allegrex-v1")

    def test_iso_inspection_unsupported_title(self) -> None:
        iso_file = self.temp_dir / "unsupported.iso"
        _create_mock_iso(iso_file, disc_id="ULUS12345", volume_id="UNSUPPORTED")

        meta = inspect_iso(iso_file)
        self.assertEqual(meta.disc_id, "ULUS12345")
        self.assertFalse(meta.is_supported)
        self.assertIsNone(meta.matched_profile)

    def test_unknown_revision_requires_an_explicit_local_identity(self) -> None:
        from nk_core.package_cache import build_title_input_identity, write_local_title_input_identity
        from test_iso_parity import create_test_iso
        import title_manifest

        manifest = _legacy_retail_manifest()
        manifest["id"] = "synthetic-revision-profile"
        manifest["disc"] = {
            "id": "TEST00001",
            "region": "OTHER",
            "revision_policy": "exact-disc-id",
            "require_local_compatibility_record": True,
        }
        manifest_dir = self.temp_dir / "manifests"
        manifest_dir.mkdir()
        manifest_path = manifest_dir / "revision.json"
        manifest_path.write_text(
            title_manifest.canonical_json(title_manifest.validate_manifest(manifest)),
            encoding="utf-8",
        )
        registry = TitleRegistry(include_defaults=False)
        registry.load_from_directory(manifest_dir, enforce_public_policy=False)

        user_root = self.temp_dir / "user-data"
        known_identity = build_title_input_identity(
            manifest=manifest,
            executable_name="EBOOT.BIN",
            executable_sha256="a" * 64,
            modules=[],
            disc_id="TEST00001",
            region="OTHER",
            disc_version="1.00",
            param_sfo={
                "DISC_ID": "TEST00001", "TITLE": "Test Game", "DISC_VERSION": "1.00",
            },
        )
        write_local_title_input_identity(user_root, known_identity)

        unknown_iso = self.temp_dir / "unknown-revision.iso"
        create_test_iso(unknown_iso, disc_id="TEST00001", version="1.01")
        metadata = inspect_iso(unknown_iso, registry, user_data_root=user_root)
        self.assertFalse(metadata.is_supported)
        self.assertIn("Unqualified revision", metadata.qualification_error)
        self.assertIn("SFO revision changed", metadata.qualification_error)
        self.assertIn("--register-local-compatibility-record", metadata.qualification_error)
        self.assertNotIn("#315", metadata.qualification_error)

    def test_package_refusals_carry_remediation_not_stale_pointer(self) -> None:
        """The CLI and planner refusals name the remediation flag and no retired #315 pointer."""
        tools_dir = Path(__file__).resolve().parent
        cli_source = (tools_dir / "nk_cli.py").read_text(encoding="utf-8")
        plan_source = (tools_dir / "title_codegen_plan.py").read_text(encoding="utf-8")
        for source in (cli_source, plan_source):
            self.assertNotIn("(#315)", source)
            self.assertNotIn("revision qualification is in the works", source)
        for refusal in (
            "Unqualified revision (SFO DISC_VERSION",
            "Local compatibility record mismatch: ",
        ):
            start = cli_source.index(refusal)
            end = cli_source.index(")\n", start)
            self.assertIn("--register-local-compatibility-record", cli_source[start:end])
        self.assertIn("write packages under the per-user data root", plan_source)

    def test_iso_inspection_malformed(self) -> None:
        # File too small
        tiny_file = self.temp_dir / "tiny.iso"
        tiny_file.write_bytes(b"short data")
        with self.assertRaises(IsoInspectionError):
            inspect_iso(tiny_file)

        # Missing PVD magic
        bad_pvd = self.temp_dir / "bad_pvd.iso"
        bad_pvd.write_bytes(b"\0" * (2 * 1024 * 1024))
        with self.assertRaises(IsoInspectionError):
            inspect_iso(bad_pvd)

    def test_preparation_engine_transactional_flow(self) -> None:
        iso_file = self.temp_dir / "synthetic.iso"
        _create_mock_iso(iso_file, disc_id="TEST00001")

        dest_root = self.temp_dir / "installed_games"
        engine = PreparationEngine(base_dir=self.temp_dir)

        events: list[ProgressEvent] = []
        result = engine.prepare_game(
            iso_file,
            on_progress=events.append,
            destination_root=dest_root,
        )

        self.assertTrue(result.success)
        self.assertIsNotNone(result.prepared_root)
        self.assertTrue(result.prepared_root.is_dir())
        self.assertTrue(result.manifest_path.is_file())

        # Verify manifest content
        with open(result.manifest_path, "r", encoding="utf-8") as f:
            manifest = json.load(f)
        self.assertEqual(manifest["disc_id"], "TEST00001")
        self.assertEqual(manifest["title_id"], "synthetic-allegrex-v1")
        self.assertEqual(manifest["game_name"], "synthetic")

        # A manifest alone is not evidence of preparation (#374). The staged
        # outputs the engine must verify before promotion: the ISO reference
        # and an attributable manifest tied to this disc, profile and source.
        disc_dir = result.prepared_root / "disc"
        self.assertTrue(disc_dir.is_dir())
        game_iso = disc_dir / "game.iso"
        iso_pointer = disc_dir / "iso_path.txt"
        self.assertTrue(game_iso.exists() or iso_pointer.is_file())
        if iso_pointer.is_file():
            self.assertEqual(
                Path(iso_pointer.read_text(encoding="utf-8").strip()), iso_file
            )
        self.assertEqual(manifest["archive_format"], "raw")
        self.assertEqual(manifest["iso_path"], str(iso_file))
        self.assertEqual(manifest["iso_size"], iso_file.stat().st_size)
        self.assertEqual(result.prepared_root.name, "TEST00001")

        # Verify no staging directories were left behind
        staging_dirs = list(dest_root.glob(".staging_*"))
        self.assertEqual(len(staging_dirs), 0)

        # Verify progress event sequence
        stages = [e.stage.value for e in events]
        self.assertIn("INSPECTING_ISO", stages)
        self.assertIn("READY", stages)

    def test_preparation_records_the_selected_boot_executable(self) -> None:
        from test_iso_parity import build_plain_mips_elf, create_test_iso_with_executables

        iso_file = self.temp_dir / "synthetic-with-eboot.iso"
        create_test_iso_with_executables(
            iso_file, build_plain_mips_elf(), disc_id="TEST00001"
        )
        result = PreparationEngine(base_dir=self.temp_dir).prepare_game(
            iso_file, destination_root=self.temp_dir / "installed_games"
        )
        self.assertTrue(result.success)
        with result.manifest_path.open("r", encoding="utf-8") as stream:
            manifest = json.load(stream)
        self.assertEqual(manifest["boot_executable"], "EBOOT.BIN")

    def test_preparation_preserves_compatible_revision_identity(self) -> None:
        """A disc matched through a compatible revision keeps its own identity.

        Preparation used to name the target directory, the manifest disc_id and
        the result after profile.disc_ids[0] no matter which ID the disc
        actually carried. Two compatible revisions therefore installed over one
        another and both claimed to be the primary disc, so the revision the
        user prepared could not be told apart from -- or could silently replace
        -- the other one.
        """
        registry = TitleRegistry(include_defaults=True)
        base = registry.lookup_by_disc_id("TEST00001")
        self.assertIsNotNone(base)

        # Same title, now also matching a second (compatible) disc ID.
        registry.register(
            TitleProfile(
                id=base.id,
                name=base.name,
                disc_ids=["TEST00001", "TEST00003"],
                regions=list(base.regions),
                executable_base=base.executable_base,
                executable_entry=base.executable_entry,
                fallback_entry=base.fallback_entry,
                required_modules=list(base.required_modules),
                archive_format=base.archive_format,
                archive_relpath=base.archive_relpath,
                save_namespace=base.save_namespace,
                runtime_profile=base.runtime_profile,
                codegen_profile=base.codegen_profile,
                min_iso_bytes=base.min_iso_bytes,
            )
        )

        dest_root = self.temp_dir / "installed_games"
        engine = PreparationEngine(base_dir=self.temp_dir, registry=registry)

        primary_iso = self.temp_dir / "primary.iso"
        _create_mock_iso(primary_iso, disc_id="TEST00001")
        revision_iso = self.temp_dir / "revision.iso"
        _create_mock_iso(revision_iso, disc_id="TEST00003")

        primary = engine.prepare_game(primary_iso, destination_root=dest_root)
        revision = engine.prepare_game(revision_iso, destination_root=dest_root)

        self.assertTrue(primary.success)
        self.assertTrue(revision.success)

        # Each revision keeps the disc ID that was actually read from the disc.
        self.assertEqual(primary.disc_id, "TEST00001")
        self.assertEqual(revision.disc_id, "TEST00003")

        # And lands in its own directory rather than overwriting the other.
        self.assertNotEqual(primary.prepared_root, revision.prepared_root)
        self.assertTrue(primary.prepared_root.is_dir())
        self.assertTrue(revision.prepared_root.is_dir())
        self.assertEqual(primary.prepared_root.name, "TEST00001")
        self.assertEqual(revision.prepared_root.name, "TEST00003")

        with open(revision.manifest_path, "r", encoding="utf-8") as f:
            manifest = json.load(f)
        self.assertEqual(manifest["disc_id"], "TEST00003")
        self.assertEqual(manifest["title_id"], base.id)

    def test_preparation_rejects_disc_id_outside_the_matched_profile(self) -> None:
        """The disc-supplied ID becomes a directory name, so it stays on a known set.

        PARAM.SFO is untrusted input. Preparation only accepts an ID the matched
        profile already declares, which is what keeps a hostile disc from
        steering the install path.
        """
        registry = TitleRegistry(include_defaults=True)
        engine = PreparationEngine(base_dir=self.temp_dir, registry=registry)

        # A disc ID that normalizes onto TEST00001 but is not the declared form.
        iso_file = self.temp_dir / "aliased.iso"
        _create_mock_iso(iso_file, disc_id="TEST-00001")

        dest_root = self.temp_dir / "installed_games"
        result = engine.prepare_game(iso_file, destination_root=dest_root)

        self.assertTrue(result.success)
        # Falls back to the profile's declared primary rather than using the
        # disc's own spelling as a path component.
        self.assertEqual(result.disc_id, "TEST00001")
        self.assertEqual(result.prepared_root.name, "TEST00001")

    def test_preparation_engine_cancellation(self) -> None:
        iso_file = self.temp_dir / "synthetic.iso"
        _create_mock_iso(iso_file, disc_id="TEST00001")

        dest_root = self.temp_dir / "installed_games"
        engine = PreparationEngine(base_dir=self.temp_dir)

        token = CancellationToken()
        # Cancel before starting
        token.cancel()

        result = engine.prepare_game(
            iso_file,
            token=token,
            destination_root=dest_root,
        )

        self.assertFalse(result.success)
        self.assertEqual(result.error_code, "PREPARATION_CANCELLED")
        # Ensure no target directory was created
        target_dir = dest_root / "TEST00001"
        self.assertFalse(target_dir.exists())

    def test_runtime_launcher_plan_construction(self) -> None:
        game_dir = self.temp_dir / "TEST00001"
        game_dir.mkdir(parents=True, exist_ok=True)
        mock_iso = self.temp_dir / "test.iso"
        mock_iso.write_bytes(b"mock_iso_content")

        manifest_file = game_dir / "manifest.json"
        manifest_data = {
            "title_id": "synthetic-allegrex-v1",
            "disc_id": "TEST00001",
            "iso_path": str(mock_iso),
            "boot_executable": "EBOOT.OLD",
        }
        manifest_file.write_text(json.dumps(manifest_data), encoding="utf-8")

        # The plan can only launch the selected title's own build product;
        # there is no retail-title executable/image fallback (#366).
        mock_exe, mock_image = _write_title_runtime(
            self.temp_dir, "synthetic", exe_ext=".exe"
        )
        _write_stale_other_title_artifacts(self.temp_dir)

        launcher = RuntimeLauncher(repo_root=self.temp_dir)
        cmd, env = launcher.build_launch_plan(
            game_dir,
            profile="Benchmark",
            fps_cap=60,
        )

        self.assertEqual(cmd[0], str(mock_exe))
        assert mock_image is not None
        self.assertEqual(cmd[2], str(mock_image))
        self.assertEqual(env["PSP_ISO"], str(mock_iso))
        self.assertEqual(env["SR_BOOT_PATH"], "disc0:/PSP_GAME/SYSDIR/EBOOT.OLD")
        self.assertEqual(env["SR_FPS_CAP"], "60")
        self.assertEqual(env["SR_GPU_GE"], "1")
        self.assertEqual(env["SR_DEBUG"], "0x20")

        # src/rt/driver.c takes image mode as
        #   --image <image> <base-hex> <entry-hex> <ref-trace> <out-trace> [--sched]
        # and exits through its argc < 4 usage path for anything shorter.
        self.assertEqual(cmd[1], "--image")
        self.assertEqual(cmd[2], str(mock_image))
        self.assertGreaterEqual(len(cmd), 7)
        self.assertTrue(cmd[3].startswith("0x"))
        self.assertTrue(cmd[4].startswith("0x"))
        self.assertIn(cmd[-1], ("--gui", "--sched"))

        # The addresses are the catalog's, not invented here.
        registry = TitleRegistry(include_defaults=True)
        profile = registry.lookup_by_disc_id("TEST00001")
        self.assertEqual(int(cmd[3], 16), profile.executable_base)
        # The run entry: a declared runtime_bindings.fallback_entry, else
        # executable.entry (title_codegen_plan._resolve_run_entry, and the native
        # catalog's run_entry), so every launcher starts the title at one address.
        self.assertEqual(int(cmd[4], 16), int(profile.fallback_entry, 16))

    def test_runtime_launcher_does_not_inherit_parent_iso_without_session_iso(self) -> None:
        game_dir = self.temp_dir / "TEST00001"
        game_dir.mkdir()
        _write_title_runtime(self.temp_dir, "synthetic", exe_ext=".exe")
        manifest_file = game_dir / "manifest.json"
        launcher = RuntimeLauncher(repo_root=self.temp_dir)

        for iso_field in ({}, {"iso_path": ""}, {"iso_path": None}):
            with self.subTest(iso_field=iso_field):
                manifest_file.write_text(
                    json.dumps({
                        "title_id": "synthetic-allegrex-v1",
                        "disc_id": "TEST00001",
                        "boot_executable": "EBOOT.OLD",
                        **iso_field,
                    }),
                    encoding="utf-8",
                )
                with patch.dict(os.environ, {
                    "PSP_ISO": "poisoned-parent.iso",
                    "SR_BOOT_PATH": "poisoned-parent-path",
                    "NK_UNRELATED_SENTINEL": "preserved",
                }):
                    _, env = launcher.build_launch_plan(game_dir)
                self.assertIsNone(env.get("PSP_ISO"))
                self.assertIsNone(env.get("SR_BOOT_PATH"))
                self.assertEqual(env["NK_UNRELATED_SENTINEL"], "preserved")

        resolved_iso = self.temp_dir / "resolved.iso"
        resolved_iso.write_bytes(b"synthetic")
        manifest_file.write_text(json.dumps({
            "title_id": "synthetic-allegrex-v1",
            "disc_id": "TEST00001",
            "iso_path": str(resolved_iso),
        }), encoding="utf-8")
        with patch.dict(os.environ, {"PSP_ISO": "poisoned-parent.iso"}):
            _, env = launcher.build_launch_plan(game_dir)
        self.assertEqual(env["PSP_ISO"], str(resolved_iso))
        self.assertIsNone(env.get("SR_BOOT_PATH"))

    def test_runtime_launcher_missing_image_fails_closed(self) -> None:
        """No image means no runnable plan, and saying so beats a usage exit."""
        game_dir = self.temp_dir / "TEST00001"
        game_dir.mkdir(parents=True, exist_ok=True)
        mock_iso = self.temp_dir / "test.iso"
        mock_iso.write_bytes(b"mock_iso_content")

        (game_dir / "manifest.json").write_text(
            json.dumps(
                {
                    "title_id": "synthetic-allegrex-v1",
                    "disc_id": "TEST00001",
                    "iso_path": str(mock_iso),
                }
            ),
            encoding="utf-8",
        )
        # The title's own runtime exists, and every legacy retail-image probe
        # location is populated: none of them may stand in for this title's
        # missing image (#366 hostile case: sibling-title image rescue).
        _write_title_runtime(self.temp_dir, "synthetic", exe_ext=".exe", image=False)
        _write_stale_other_title_artifacts(self.temp_dir)

        launcher = RuntimeLauncher(repo_root=self.temp_dir)
        with self.assertRaises(RuntimeLaunchError) as cm:
            launcher.build_launch_plan(game_dir)
        msg = str(cm.exception).replace(str(self.temp_dir), "<TEMP_DIR>")
        self.assertIn("Runtime image not found", msg)
        self.assertNotIn("hst", msg)

    def test_runtime_launcher_resolves_extensionless_binary(self) -> None:
        """The runtime has no .exe suffix on Linux or macOS."""
        game_dir = self.temp_dir / "TEST00001"
        game_dir.mkdir(parents=True, exist_ok=True)
        mock_iso = self.temp_dir / "test.iso"
        mock_iso.write_bytes(b"mock_iso_content")

        (game_dir / "manifest.json").write_text(
            json.dumps(
                {
                    "title_id": "synthetic-allegrex-v1",
                    "disc_id": "TEST00001",
                    "iso_path": str(mock_iso),
                }
            ),
            encoding="utf-8",
        )
        posix_exe, posix_img = _write_title_runtime(self.temp_dir, "synthetic")
        _write_stale_other_title_artifacts(self.temp_dir)

        launcher = RuntimeLauncher(repo_root=self.temp_dir)
        cmd, _ = launcher.build_launch_plan(game_dir)
        self.assertEqual(cmd[0], str(posix_exe))
        assert posix_img is not None
        self.assertEqual(cmd[2], str(posix_img))

    def test_runtime_launcher_honors_validated_title_game_name(self) -> None:
        """The build name comes from the validated title identity.

        The session manifest may only *restate* the validated game name; the
        Python and native planners both key their candidates off the one
        registry/catalog value so they cannot diverge (#366).
        """
        game_dir = self.temp_dir / "TEST00001"
        game_dir.mkdir(parents=True, exist_ok=True)
        mock_iso = self.temp_dir / "test.iso"
        mock_iso.write_bytes(b"mock_iso_content")

        (game_dir / "manifest.json").write_text(
            json.dumps(
                {
                    "title_id": "synthetic-allegrex-v1",
                    "game_name": "synthetic",
                    "disc_id": "TEST00001",
                    "iso_path": str(mock_iso),
                }
            ),
            encoding="utf-8",
        )
        build_dir = self.temp_dir / "build" / "synthetic"
        build_dir.mkdir(parents=True, exist_ok=True)
        mock_exe = build_dir / "synthetic.exe"
        mock_exe.write_bytes(b"MZfake")
        mock_image = build_dir / "synthetic_image.bin"
        mock_image.write_bytes(b"image")

        launcher = RuntimeLauncher(repo_root=self.temp_dir)
        cmd, _ = launcher.build_launch_plan(game_dir)
        self.assertEqual(cmd[0], str(mock_exe))
        self.assertEqual(cmd[2], str(mock_image))

    def test_runtime_launcher_missing_binary(self) -> None:
        game_dir = self.temp_dir / "TEST00001"
        game_dir.mkdir(parents=True, exist_ok=True)
        manifest_file = game_dir / "manifest.json"
        manifest_file.write_text(json.dumps({"title_id": "test"}), encoding="utf-8")

        launcher = RuntimeLauncher(repo_root=self.temp_dir)
        with self.assertRaises(RuntimeLaunchError):
            launcher.build_launch_plan(game_dir)

    def test_runtime_launcher_moved_iso_error(self) -> None:
        game_dir = self.temp_dir / "TEST00001"
        game_dir.mkdir(parents=True, exist_ok=True)
        manifest_file = game_dir / "manifest.json"
        manifest_data = {
            "title_id": "synthetic-allegrex-v1",
            "disc_id": "TEST00001",
            "iso_path": str(self.temp_dir / "nonexistent" / "moved.iso"),
        }
        manifest_file.write_text(json.dumps(manifest_data), encoding="utf-8")

        _write_title_runtime(self.temp_dir, "synthetic", exe_ext=".exe")

        launcher = RuntimeLauncher(repo_root=self.temp_dir)
        with self.assertRaises(RuntimeLaunchError) as cm:
            launcher.build_launch_plan(game_dir)
        self.assertIn("Game source ISO not found", str(cm.exception))

    def test_runtime_launcher_moved_iso_with_fallback(self) -> None:
        game_dir = self.temp_dir / "TEST00001"
        game_dir.mkdir(parents=True, exist_ok=True)

        # Fallback exists in disc/game.iso
        fallback_iso = game_dir / "disc" / "game.iso"
        fallback_iso.parent.mkdir(parents=True, exist_ok=True)
        fallback_iso.write_bytes(b"fallback_iso_content")

        manifest_file = game_dir / "manifest.json"
        manifest_data = {
            "title_id": "synthetic-allegrex-v1",
            "disc_id": "TEST00001",
            "iso_path": str(self.temp_dir / "old_location" / "moved.iso"),
        }
        manifest_file.write_text(json.dumps(manifest_data), encoding="utf-8")

        _write_title_runtime(self.temp_dir, "synthetic", exe_ext=".exe")

        launcher = RuntimeLauncher(repo_root=self.temp_dir)
        cmd, env = launcher.build_launch_plan(game_dir)
        self.assertEqual(env["PSP_ISO"], str(fallback_iso))

    def test_invalid_paths(self) -> None:
        # Nonexistent path
        with self.assertRaises(IsoInspectionError):
            inspect_iso(self.temp_dir / "ghost_file.iso")

        # Directory instead of file
        sub_dir = self.temp_dir / "some_directory"
        sub_dir.mkdir()
        with self.assertRaises(IsoInspectionError):
            inspect_iso(sub_dir)

    def test_unicode_paths(self) -> None:
        unicode_dir = self.temp_dir / "フォルダ_日本語_ポータブル"
        unicode_dir.mkdir(parents=True, exist_ok=True)
        unicode_iso = unicode_dir / "みんなのテニス_ポルトガル語_áéíóú.iso"
        _create_mock_iso(unicode_iso, disc_id="TEST00001", volume_id="MINNA_TENNIS")

        meta = inspect_iso(unicode_iso)
        self.assertEqual(meta.disc_id, "TEST00001")
        self.assertTrue(meta.is_supported)

        engine = PreparationEngine(base_dir=self.temp_dir)
        dest_root = self.temp_dir / "日本語_installed"
        result = engine.prepare_game(unicode_iso, destination_root=dest_root)
        self.assertTrue(result.success)
        self.assertTrue(result.prepared_root.is_dir())
        self.assertTrue(result.manifest_path.is_file())

    def test_paths_with_spaces(self) -> None:
        spaced_dir = self.temp_dir / "My PSP Games Collection" / "Sub Folder"
        spaced_dir.mkdir(parents=True, exist_ok=True)
        spaced_iso = spaced_dir / "Hot Shots Tennis (USA) v1.0.iso"
        _create_mock_iso(spaced_iso, disc_id="TEST00001", volume_id="HST_USA")

        meta = inspect_iso(spaced_iso)
        self.assertEqual(meta.disc_id, "TEST00001")
        self.assertTrue(meta.is_supported)

        engine = PreparationEngine(base_dir=self.temp_dir)
        dest_root = self.temp_dir / "Games Library Destination"
        result = engine.prepare_game(spaced_iso, destination_root=dest_root)
        self.assertTrue(result.success)
        self.assertEqual(result.prepared_root.name, "TEST00001")

        # Verify launch plan preserves spaces: the title's own runtime lives
        # under a spaced repository root and the plan resolves it there.
        mock_exe, mock_img = _write_title_runtime(self.temp_dir, "synthetic", exe_ext=".exe")
        launcher = RuntimeLauncher(repo_root=self.temp_dir)
        cmd, env = launcher.build_launch_plan(result.prepared_root)
        self.assertEqual(env["PSP_ISO"], str(spaced_iso))
        # The plan is runnable rather than a bare executable that would reach
        # the runtime's usage exit.
        self.assertEqual(cmd[1], "--image")
        self.assertEqual(cmd[0], str(mock_exe))
        assert mock_img is not None
        self.assertEqual(cmd[2], str(mock_img))

    def test_game_library_persistence(self) -> None:
        lib_file = self.temp_dir / "library.json"
        lib = GameLibrary(storage_file=lib_file)
        self.assertEqual(lib.count(), 0)

        # Create real mock files for source checking
        real_iso = self.temp_dir / "real.iso"
        real_iso.write_bytes(b"dummy")
        missing_iso = self.temp_dir / "missing.iso"

        rec1 = LibraryGameRecord(
            disc_id="TEST00001",
            title_name="Synthetic Allegrex Test",
            iso_path=str(real_iso),
            is_prepared=True,
            settings_override={"resolution_scale": 4, "fps_cap": 60},
        )
        rec2 = LibraryGameRecord(
            disc_id="TEST00002",
            title_name="Synthetic Secondary Test",
            iso_path=str(missing_iso),
            is_prepared=False,
        )

        lib.add_or_update_game(rec1)
        lib.add_or_update_game(rec2)
        self.assertEqual(lib.count(), 2)

        # Check source verification
        sources = lib.verify_sources()
        self.assertTrue(sources["TEST00001"])
        self.assertFalse(sources["TEST00002"])

        # Save to disk
        lib.save()
        self.assertTrue(lib_file.is_file())

        # Load from disk and verify data integrity
        loaded = GameLibrary.load(lib_file)
        self.assertEqual(loaded.count(), 2)

        g1 = loaded.get_game("TEST-00001") # test normalized lookup
        self.assertIsNone(g1) # Direct key lookup is exact normalized disc_id
        g1 = loaded.get_game("TEST00001")
        self.assertIsNotNone(g1)
        self.assertEqual(g1.title_name, "Synthetic Allegrex Test")
        self.assertTrue(g1.is_prepared)
        self.assertEqual(g1.settings_override["resolution_scale"], 4)
        self.assertEqual(g1.settings_override["fps_cap"], 60)

        # Test remove
        self.assertTrue(loaded.remove_game("TEST00002"))
        self.assertEqual(loaded.count(), 1)
        self.assertFalse(loaded.remove_game("NONEXISTENT"))


# ---------------------------------------------------------------------------
# #366 Generic launcher identity/path contract.
#
# Generic launch resolution derives title identity, runtime executable, image,
# and base/entry ONLY from validated title/manifest/session data. The tests
# below are hostile fixtures: a stale sibling-title (retail-shaped) build in
# the workspace must be irrelevant to every other title, missing identity must
# fail closed with an actionable diagnostic, and the native and Python
# planners must select the same outcome for identical source-owned fixtures.
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parent.parent

# Tokens that may never reappear as generic launcher defaults or candidates.
# The check is deliberately narrow: it targets identity defaults and path
# probes, so legitimate test/history/private-profile references elsewhere in
# the tree are not rejected.
RETAIL_FALLBACK_TOKENS = (
    "UCUS98701",
    '"hst"',
    "'hst'",
    "hst_image",
    "build/hst",
    "%chst",
    "nakagawa_runtime",
)

GENERIC_LAUNCHER_SOURCES = (
    "tools/nk_core/launcher.py",
    "tools/title_catalog_codegen.py",
    "src/core/nk_launch.c",
    "src/core/generated/nk_title_catalog.h",
    "src/core/generated/nk_title_catalog.c",
)


def _retail_fallback_findings(text: str) -> list[str]:
    """Tokens in `text` that look like retail-title generic fallbacks."""
    return [token for token in RETAIL_FALLBACK_TOKENS if token in text]


def _legacy_retail_manifest() -> dict:
    """Source-owned stand-in for an explicitly retail-identified title.

    It mirrors the shape of the local legacy retail title manifest (zero base
    and entry are the declared launch values) without carrying any private
    byte or private address: the addresses here are synthetic.
    """
    base = json.loads(
        (REPO_ROOT / "assets" / "titles" / "synthetic.json").read_text(encoding="utf-8")
    )
    base["id"] = "hst-ucus98701-v1"
    base["display_name"] = "Legacy Retail Fixture (synthetic stand-in)"
    base["kind"] = "retail"
    base["game_name"] = "hst"
    base["disc"] = {
        "id": "UCUS98701",
        "region": "NA",
        "revision_policy": "exact-disc-id",
    }
    base["executable"]["base"] = 0
    base["executable"]["entry"] = 0
    # profile_zero is a synthetic-kind-only key; a retail-identified manifest
    # carries its identity through kind/disc/game_name, not the public
    # synthetic surface.
    base.pop("profile_zero", None)
    return base


def _load_python_module_from_source(name: str, source: str, package: str = "nk_core"):
    """Exec an in-memory variant of a package module for mutation tests."""
    import types

    module = types.ModuleType(name)
    module.__package__ = package
    module.__file__ = f"<{name}>"
    exec(compile(source, module.__file__, "exec"), module.__dict__)  # noqa: S102
    return module


class BootExecutablePathLimitTests(unittest.TestCase):
    def test_psp_boot_path_limit_is_255_utf8_bytes(self) -> None:
        from nk_core.launcher import psp_boot_path

        valid = "é" * 127 + "A"
        self.assertEqual(len(valid.encode("utf-8")), 255)
        self.assertEqual(
            psp_boot_path(valid), f"disc0:/PSP_GAME/SYSDIR/{valid}"
        )
        self.assertIsNone(psp_boot_path("é" * 128))


class GenericLauncherHostileTests(unittest.TestCase):
    """Hostile contract cases for the Python generic launcher (#366)."""

    TITLE2 = "pspdev-phase5-v1"
    TITLE2_DISC = "TEST00005"
    TITLE2_GAME = "pspdev-phase5"

    def setUp(self) -> None:
        self.temp_dir = Path(tempfile.mkdtemp(prefix="nk_launch_genericity_"))
        self.iso = self.temp_dir / "source.iso"
        self.iso.write_bytes(b"iso")

    def tearDown(self) -> None:
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def _game_dir(self, data, name: str = "TEST00005") -> Path:
        game_dir = self.temp_dir / name
        game_dir.mkdir(exist_ok=True)
        payload = dict(data)
        payload.setdefault("iso_path", str(self.iso))
        (game_dir / "manifest.json").write_text(json.dumps(payload), encoding="utf-8")
        return game_dir

    def _plan(self, data, name: str = "TEST00005"):
        launcher = RuntimeLauncher(repo_root=self.temp_dir)
        return launcher.build_launch_plan(self._game_dir(data, name=name))

    def _title2_manifest(self, **extra):
        data = {"title_id": self.TITLE2, "disc_id": self.TITLE2_DISC}
        data.update(extra)
        return data

    def _error(self, data, name: str = "TEST00005") -> str:
        with self.assertRaises(RuntimeLaunchError) as cm:
            self._plan(data, name=name)
        return str(cm.exception)

    # Hostile 1: a manifest with no identity must fail closed, not inherit a
    # retail default identity, even when a stale retail build is present.
    def test_missing_identity_fails_closed_without_retail_default(self) -> None:
        _write_stale_other_title_artifacts(self.temp_dir)
        message = self._error({}, name="EMPTY_ID")
        self.assertIn("identity", message.lower())
        self.assertNotIn("UCUS98701", message)
        self.assertNotIn("hst", message)

    # Hostile 2: an identity that does not resolve in the validated registry is
    # an actionable validation error, not a probe list containing retail paths.
    def test_unvalidated_disc_identity_fails_closed(self) -> None:
        message = self._error({"disc_id": "ZZZZ99999"})
        self.assertIn("No title profile", message)
        self.assertNotIn("build/hst", message)

    def test_disc_identity_alone_is_validated_and_sufficient(self) -> None:
        exe, _ = _write_title_runtime(self.temp_dir, self.TITLE2_GAME, exe_ext=".exe")
        cmd, _ = self._plan({"disc_id": self.TITLE2_DISC})
        self.assertEqual(Path(cmd[0]), exe)

    # Hostile 3: a stale build/<other> runtime must not win for another title.
    def test_stale_other_title_build_cannot_satisfy_second_title(self) -> None:
        _write_stale_other_title_artifacts(self.temp_dir)
        message = self._error(self._title2_manifest())
        self.assertIn("Runtime binary not found", message)
        self.assertNotIn("hst", message)

    # Hostile 4: a retail-shaped binary in the repository root is not a
    # candidate for any other title.
    def test_root_retail_binary_cannot_satisfy_second_title(self) -> None:
        (self.temp_dir / "hst").write_bytes(b"ELFfake")
        (self.temp_dir / "hst.exe").write_bytes(b"MZfake")
        (self.temp_dir / "hst_image.bin").write_bytes(b"image")
        message = self._error(self._title2_manifest())
        self.assertIn("Runtime binary not found", message)
        self.assertNotIn("hst", message)

    # Hostile 5: retail-specific image candidates cannot satisfy another title.
    def test_other_title_image_cannot_satisfy_second_title(self) -> None:
        _write_title_runtime(self.temp_dir, self.TITLE2_GAME, exe_ext=".exe", image=False)
        _write_stale_other_title_artifacts(self.temp_dir)
        exe_dir = self.temp_dir / "build" / self.TITLE2_GAME
        (exe_dir / "hst_image.bin").write_bytes(b"image")
        message = self._error(self._title2_manifest())
        self.assertIn("Runtime image not found", message)
        self.assertNotIn("hst", message)

    # Hostile 6: the correct title runtime succeeds, even while stale retail
    # artifacts exist elsewhere in the workspace.
    def test_correct_title_runtime_launches_its_own_build(self) -> None:
        exe, img = _write_title_runtime(self.temp_dir, self.TITLE2_GAME, exe_ext=".exe")
        _write_stale_other_title_artifacts(self.temp_dir)
        cmd, env = self._plan(self._title2_manifest())
        assert img is not None
        self.assertEqual(Path(cmd[0]), exe)
        self.assertEqual(Path(cmd[2]), img)
        self.assertEqual(env["PSP_ISO"], str(self.iso))

    def test_loose_root_environment_uses_manifest_and_masks_inherited_value(self) -> None:
        source = json.loads(
            (REPO_ROOT / "assets" / "titles" / "synthetic.json").read_text(encoding="utf-8")
        )
        source["id"] = "launch-loose-roots-v1"
        source["game_name"] = "launch-loose-roots"
        source["display_name"] = "Launch Loose Roots"
        source["filesystem"]["data_root"] = "extracted"
        source["filesystem"]["loose_content_roots"] = [
            {"root": "loose", "mount": "", "precedence": 7},
        ]
        source_path = self.temp_dir / "loose-roots-title.json"
        source_path.write_text(json.dumps(source), encoding="utf-8")
        registry = TitleRegistry(include_defaults=False)
        registry.load_private_manifest(source_path)

        game_dir = self._game_dir({"title_id": source["id"]}, name="LOOSE_ROOTS")
        data_root = game_dir / "extracted"
        loose_root = game_dir / "loose"
        data_root.mkdir()
        loose_root.mkdir()
        exe, _ = _write_title_runtime(self.temp_dir, source["game_name"], exe_ext=".exe")

        with patch.dict(
            os.environ,
            {"SR_DATAROOT": "inherited-data-root", "SR_LOOSE_CONTENT_ROOTS": "inherited-root"},
        ):
            cmd, env = RuntimeLauncher(
                repo_root=self.temp_dir, registry=registry
            ).build_launch_plan(game_dir)
        expected = title_manifest.encode_loose_content_roots(
            title_manifest.validate_manifest(source), data_root.resolve()
        )
        self.assertEqual(Path(cmd[0]), exe)
        self.assertEqual(env["SR_DATAROOT"], str(data_root))
        self.assertEqual(env["SR_LOOSE_CONTENT_ROOTS"], expected)
        self.assertNotEqual(env["SR_LOOSE_CONTENT_ROOTS"], "inherited-root")

        shutil.rmtree(loose_root)
        with patch.dict(
            os.environ,
            {"SR_DATAROOT": "inherited-data-root", "SR_LOOSE_CONTENT_ROOTS": "inherited-root"},
        ):
            with self.assertRaisesRegex(RuntimeLaunchError, r"Loose-content root binding:"):
                RuntimeLauncher(repo_root=self.temp_dir, registry=registry).build_launch_plan(game_dir)

    # Hostile 7: a session whose disc and title identities disagree is rejected
    # before any plan (and therefore before any spawn) is produced.
    def test_wrong_title_package_identity_rejected_before_plan(self) -> None:
        _write_title_runtime(self.temp_dir, "synthetic", exe_ext=".exe")
        message = self._error(
            {"title_id": "synthetic-allegrex-v1", "disc_id": self.TITLE2_DISC}
        )
        self.assertIn("identity", message.lower())
        self.assertNotIn("hst", message)

    # Hostile 8: multiple valid candidates resolve in an explicit deterministic
    # contract order: manager game_name before title id, .exe before the
    # extensionless spelling within one name.
    def test_ambiguous_valid_candidates_resolve_in_contract_order(self) -> None:
        by_game, _ = _write_title_runtime(self.temp_dir, self.TITLE2_GAME, exe_ext=".exe")
        by_title, _ = _write_title_runtime(self.temp_dir, self.TITLE2, exe_ext=".exe")
        self.assertNotEqual(by_game, by_title)
        cmd, _ = self._plan(self._title2_manifest())
        self.assertEqual(Path(cmd[0]), by_game)
        # Within one name the Windows spelling is probed before extensionless.
        by_game.with_suffix("").write_bytes(b"ELFfake")
        cmd, _ = self._plan(self._title2_manifest())
        self.assertEqual(Path(cmd[0]), by_game)

    # Hostile 9: a malformed manifest cannot inherit any identity.
    def test_malformed_manifest_fails_closed(self) -> None:
        game_dir = self.temp_dir / "MALFORMED"
        game_dir.mkdir(exist_ok=True)
        (game_dir / "manifest.json").write_text("[]", encoding="utf-8")
        with self.assertRaises(RuntimeLaunchError) as cm:
            RuntimeLauncher(repo_root=self.temp_dir).build_launch_plan(game_dir)
        self.assertIn("object", str(cm.exception))

    # Hostile 10: moving the repository root or the process cwd never changes
    # the selected identity or its addresses; it only changes where a
    # correctly identified runtime can be found (or reports it missing).
    def test_repository_root_and_cwd_do_not_alter_identity(self) -> None:
        exe, _ = _write_title_runtime(self.temp_dir, self.TITLE2_GAME, exe_ext=".exe")
        game_dir = self._game_dir(self._title2_manifest())
        other_root = self.temp_dir / "other root"
        other_root.mkdir()
        _write_stale_other_title_artifacts(other_root)

        cmd_a, _ = RuntimeLauncher(repo_root=self.temp_dir).build_launch_plan(game_dir)
        with self.assertRaises(RuntimeLaunchError) as cm:
            RuntimeLauncher(repo_root=other_root).build_launch_plan(game_dir)
        self.assertIn("Runtime binary not found", str(cm.exception))
        self.assertNotIn("hst", str(cm.exception))
        self.assertEqual(Path(cmd_a[0]), exe)

        cwd = os.getcwd()
        try:
            os.chdir(other_root)
            with self.assertRaises(RuntimeLaunchError):
                RuntimeLauncher().build_launch_plan(game_dir)
            os.chdir(self.temp_dir)
            cmd_b, _ = RuntimeLauncher().build_launch_plan(game_dir)
        finally:
            os.chdir(cwd)
        # Identity-derived fields are identical across roots and cwds.
        self.assertEqual(cmd_a[3:5], cmd_b[3:5])
        self.assertEqual(Path(cmd_b[0]), exe)

    # Hostile 11: spaced roots and spaced prepared directories keep working.
    def test_paths_with_spaces_second_title_route(self) -> None:
        spaced_root = self.temp_dir / "root with spaces"
        exe, img = _write_title_runtime(spaced_root, self.TITLE2_GAME, exe_ext=".exe")
        game_dir = self._game_dir(self._title2_manifest(), name="prepared game dir")
        cmd, _ = RuntimeLauncher(repo_root=spaced_root).build_launch_plan(game_dir)
        assert img is not None
        self.assertEqual(Path(cmd[0]), exe)
        self.assertEqual(Path(cmd[2]), img)
        self.assertIn(" ", cmd[0])

    # Hostile 13: the generic API exposes no explicit-executable escape; a
    # caller cannot hand the generic planner a retail binary path directly.
    def test_generic_api_has_no_explicit_executable_escape(self) -> None:
        import inspect

        params = set(inspect.signature(RuntimeLauncher.build_launch_plan).parameters)
        self.assertEqual(
            params,
            {
                "self", "game_dir", "profile", "fps_cap", "gpu_ge",
                "software_render", "no_gui",
            },
        )

    # Legacy compatibility: an explicitly retail-identified title keeps
    # working through the generic API because its OWN validated manifest
    # identifies it (game_name and disc), not because generic code defaults to
    # it. The same session without that validated identity fails closed.
    def test_legacy_retail_identified_route_works_only_via_own_identity(self) -> None:
        from nk_core.title_registry import TitleRegistry

        manifest_path = self.temp_dir / "legacy-retail-title.json"
        manifest_path.write_text(json.dumps(_legacy_retail_manifest()), encoding="utf-8")

        registry = TitleRegistry(include_defaults=True)
        registry.load_private_manifest(manifest_path)

        exe, img = _write_title_runtime(self.temp_dir, "hst", exe_ext=".exe")
        data = {"title_id": "hst-ucus98701-v1", "disc_id": "UCUS98701"}

        # With the validated identity loaded: resolves its own build.
        launcher = RuntimeLauncher(repo_root=self.temp_dir, registry=registry)
        cmd, _ = launcher.build_launch_plan(self._game_dir(data, name="LEGACY"))
        assert img is not None
        self.assertEqual(Path(cmd[0]), exe)
        self.assertEqual(Path(cmd[2]), img)
        self.assertEqual(int(cmd[3], 16), 0)
        # Its ELF entry is 0, which dispatch resolves as a real function, so the
        # runtime's own fallback never fires: the launcher must pass the declared
        # fallback entry itself (title_codegen_plan._resolve_run_entry).
        legacy = _legacy_retail_manifest()
        self.assertEqual(int(cmd[4], 16), legacy["runtime_bindings"]["fallback_entry"])
        self.assertNotEqual(int(cmd[4], 16), legacy["executable"]["entry"])

        # Without it, generic code has no default title: fail closed.
        public_only = RuntimeLauncher(repo_root=self.temp_dir)
        with self.assertRaises(RuntimeLaunchError) as cm:
            public_only.build_launch_plan(self._game_dir(data, name="LEGACY"))
        self.assertIn("No title profile", str(cm.exception))


class GenericLauncherParityTests(unittest.TestCase):
    """Native and Python planners must select the same outcome (#366 parity)."""

    TITLE2 = "pspdev-phase5-v1"
    TITLE2_DISC = "TEST00005"
    TITLE2_GAME = "pspdev-phase5"

    HARNESS_C = r'''#include "nk_launch.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    NkGameEntry game;
    NkLaunchSession session;
    NkResult res;
    if (argc < 5) return 2;
    memset(&game, 0, sizeof(game));
    memset(&session, 0, sizeof(session));
    snprintf(game.disc_id, sizeof(game.disc_id), "%s", argv[2]);
    snprintf(game.title_id, sizeof(game.title_id), "%s", argv[3]);
    snprintf(game.iso_path, sizeof(game.iso_path), "%s", argv[4]);
    res = nk_launch_prepare_session(&session, &game, argv[1]);
    printf("RESULT:%d\n", (int)res);
    if (res == NK_OK) {
        printf("EXE:%s\n", session.executable_path);
        printf("IMAGE:%s\n", session.image_path);
        printf("BASE:%08x\n", session.base_address);
        printf("ENTRY:%08x\n", session.entry_point);
    } else {
        printf("ERROR:%s\n", session.last_error);
    }
    printf("AVAILABLE:%d\n", nk_launch_runtime_available(argv[1], argv[3]) ? 1 : 0);
    return 0;
}
'''

    @classmethod
    def setUpClass(cls) -> None:
        cls.gcc = shutil.which("gcc")
        if not cls.gcc:
            return
        cls._class_temp = Path(tempfile.mkdtemp(prefix="nk_launch_parity_class_"))
        cls._harness = cls._compile_harness_class(cls._class_temp, cls.gcc)

    @classmethod
    def tearDownClass(cls) -> None:
        if hasattr(cls, "_class_temp"):
            shutil.rmtree(cls._class_temp, ignore_errors=True)

    def setUp(self) -> None:
        if not self.gcc:
            self.skipTest("gcc not available for the native parity harness")
        self.temp_dir = Path(tempfile.mkdtemp(prefix="nk_launch_parity_"))
        self.iso = self.temp_dir / "source.iso"
        self.iso.write_bytes(b"iso")
        self.harness = self._harness

    def tearDown(self) -> None:
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    @classmethod
    def _compile_harness_class(cls, class_temp: Path, gcc: str) -> Path:
        harness_c = class_temp / "parity_harness.c"
        harness_c.write_text(cls.HARNESS_C, encoding="utf-8", newline="\n")
        out = class_temp / ("parity_harness.exe" if sys.platform == "win32" else "parity_harness")
        core_srcs = [
            REPO_ROOT / "src" / "core" / "nk_iso.c",
            REPO_ROOT / "src" / "core" / "nk_library.c",
            REPO_ROOT / "src" / "core" / "nk_launch.c",
            REPO_ROOT / "src" / "core" / "nk_title_manifest.c",
            REPO_ROOT / "src" / "core" / "nk_json.c",
            REPO_ROOT / "src" / "core" / "generated" / "nk_title_catalog.c",
        ]
        if sys.platform == "win32":
            core_srcs.append(REPO_ROOT / "src" / "core" / "nk_platform_win32.c")
        else:
            core_srcs.append(REPO_ROOT / "src" / "core" / "nk_platform_posix.c")
        cmd = [
            gcc, "-std=c99", "-Wall", "-Wextra",
            "-I", str(REPO_ROOT / "src" / "core"),
            "-I", str(REPO_ROOT / "src" / "core" / "generated"),
            str(harness_c),
        ] + [str(s) for s in core_srcs] + (
            ["-lshell32", "-lole32", "-luuid"] if sys.platform == "win32" else []
        ) + ["-o", str(out)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"failed to build native launch parity harness:\n{res.stderr}\n{res.stdout}")
        return out

    @staticmethod
    def _parse_native(output: str) -> dict[str, str]:
        parsed: dict[str, str] = {}
        for line in output.splitlines():
            if ":" in line:
                key, value = line.split(":", 1)
                parsed[key] = value
        return parsed

    def _native(self, root: Path) -> dict[str, str]:
        cmd = [
            str(self.harness), str(root), self.TITLE2_DISC, self.TITLE2, str(self.iso),
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"native harness failed: {res.stderr}")
        return self._parse_native(res.stdout)

    @staticmethod
    def _same_path(a: str, b: str) -> bool:
        return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))

    def _python_plan(self, root: Path):
        game_dir = root / "TEST00005"
        game_dir.mkdir(exist_ok=True)
        (game_dir / "manifest.json").write_text(
            json.dumps(
                {
                    "title_id": self.TITLE2,
                    "disc_id": self.TITLE2_DISC,
                    "iso_path": str(self.iso),
                }
            ),
            encoding="utf-8",
        )
        return RuntimeLauncher(repo_root=root).build_launch_plan(game_dir)

    def test_native_and_python_select_the_same_runtime_image_and_addresses(self) -> None:
        root = self.temp_dir / "ws"
        exe, img = _write_title_runtime(root, self.TITLE2_GAME, exe_ext=".exe")
        _write_stale_other_title_artifacts(root)
        assert img is not None

        cmd, _ = self._python_plan(root)
        native = self._native(root)

        self.assertEqual(native.get("RESULT"), "0", native.get("ERROR"))
        # Correctness anchor: both planners chose the selected title's own
        # build, never the stale retail artifacts in the same workspace.
        self.assertEqual(Path(cmd[0]), exe)
        self.assertTrue(self._same_path(cmd[0], native["EXE"]))
        self.assertTrue(self._same_path(cmd[2], native["IMAGE"]))
        self.assertEqual(int(cmd[3], 16), int(native["BASE"], 16))
        self.assertEqual(int(cmd[4], 16), int(native["ENTRY"], 16))
        self.assertNotIn("hst", native["EXE"].replace(str(root), ""))
        self.assertEqual(native.get("AVAILABLE"), "1")

    def test_native_and_python_fail_the_same_way_without_a_title_runtime(self) -> None:
        root = self.temp_dir / "ws_missing"
        _write_stale_other_title_artifacts(root)

        with self.assertRaises(RuntimeLaunchError) as cm:
            self._python_plan(root)
        self.assertIn("Runtime binary not found", str(cm.exception))
        self.assertNotIn("hst", str(cm.exception))

        native = self._native(root)
        self.assertNotEqual(native.get("RESULT"), "0")
        self.assertIn("Runtime binary not found", native.get("ERROR", ""))
        self.assertNotIn("hst", native.get("ERROR", ""))
        self.assertEqual(native.get("AVAILABLE"), "0")

    # Cross-planner precedence: when BOTH the game_name build and the
    # title_id build are valid candidates, native and Python must choose the
    # exact same file, and it must be the one NAME_SOURCES orders first --
    # with .exe beating the extensionless spelling within a name.
    def test_two_valid_candidates_follow_name_sources_in_both_planners(self) -> None:
        import nk_core.launcher as nk_launcher

        names_by_source = {"game_name": self.TITLE2_GAME, "title_id": self.TITLE2}
        first_src, second_src = nk_launcher.NAME_SOURCES[:2]
        self.assertIn(first_src, names_by_source)
        self.assertIn(second_src, names_by_source)
        first = names_by_source[first_src]
        second = names_by_source[second_src]

        # Case A: both builds exist with both spellings; the contract's first
        # source must win, and its .exe must win over its extensionless twin.
        root_a = self.temp_dir / "ws_two_a"
        _write_title_runtime(root_a, first, exe_ext="")
        _write_title_runtime(root_a, first, exe_ext=".exe")
        _write_title_runtime(root_a, second, exe_ext="")
        _write_title_runtime(root_a, second, exe_ext=".exe")
        expect_exe_a = root_a / "build" / first / f"{first}.exe"
        expect_img_a = root_a / "build" / first / f"{first}_image.bin"

        cmd_a, _ = self._python_plan(root_a)
        native_a = self._native(root_a)
        self.assertEqual(native_a.get("RESULT"), "0", native_a.get("ERROR"))
        self.assertTrue(self._same_path(cmd_a[0], str(expect_exe_a)), cmd_a[0])
        self.assertTrue(self._same_path(native_a["EXE"], str(expect_exe_a)), native_a["EXE"])
        self.assertTrue(self._same_path(cmd_a[0], native_a["EXE"]))
        self.assertTrue(self._same_path(cmd_a[2], str(expect_img_a)), cmd_a[2])
        self.assertTrue(self._same_path(native_a["IMAGE"], str(expect_img_a)), native_a["IMAGE"])
        self.assertTrue(self._same_path(cmd_a[2], native_a["IMAGE"]))
        self.assertEqual(native_a.get("AVAILABLE"), "1")

        # Case B: name-source order beats pattern order. The first source has
        # only the extensionless spelling while the second source has the
        # .exe; both planners must still choose the first source's binary.
        root_b = self.temp_dir / "ws_two_b"
        _write_title_runtime(root_b, first, exe_ext="")
        _write_title_runtime(root_b, second, exe_ext=".exe")
        expect_exe_b = root_b / "build" / first / first
        expect_img_b = root_b / "build" / first / f"{first}_image.bin"

        cmd_b, _ = self._python_plan(root_b)
        native_b = self._native(root_b)
        self.assertEqual(native_b.get("RESULT"), "0", native_b.get("ERROR"))
        self.assertTrue(self._same_path(cmd_b[0], str(expect_exe_b)), cmd_b[0])
        self.assertTrue(self._same_path(native_b["EXE"], str(expect_exe_b)), native_b["EXE"])
        self.assertTrue(self._same_path(cmd_b[0], native_b["EXE"]))
        self.assertTrue(self._same_path(cmd_b[2], str(expect_img_b)), cmd_b[2])
        self.assertTrue(self._same_path(native_b["IMAGE"], str(expect_img_b)), native_b["IMAGE"])
        self.assertEqual(native_b.get("AVAILABLE"), "1")

    # Tripwire: a planner-side NAME_SOURCES reorder must propagate into the
    # generated header AND native behavior. Native binds its name array only
    # through the generated index macros, so compiling nk_launch.c against a
    # regenerated reordered contract must flip the chosen candidate -- it
    # cannot silently remain on the old order -- and the Python planner must
    # flip with it. Unsupported or duplicated sources fail generation.
    def test_name_source_reorder_propagates_to_native_and_python(self) -> None:
        import nk_core.launcher as nk_launcher
        import publication_policy
        import title_catalog_codegen as tcg

        fixture_root = self.temp_dir / "ws_reorder"
        _write_title_runtime(fixture_root, self.TITLE2_GAME, exe_ext=".exe")
        _write_title_runtime(fixture_root, self.TITLE2, exe_ext=".exe")
        game_exe = fixture_root / "build" / self.TITLE2_GAME / f"{self.TITLE2_GAME}.exe"
        id_exe = fixture_root / "build" / self.TITLE2 / f"{self.TITLE2}.exe"

        original_sources = tcg.NAME_SOURCES
        original_planner = nk_launcher.NAME_SOURCES
        reversed_sources = ("title_id", "game_name")
        try:
            tcg.NAME_SOURCES = reversed_sources
            nk_launcher.NAME_SOURCES = reversed_sources

            # 1. Codegen derives the index macros from the tuple itself, and
            # the committed header therefore disagrees with the mutated
            # projection: regeneration / --verify fails closed on the drift.
            header_lines = tcg._launch_contract_header_lines()
            self.assertIn("#define NK_LAUNCH_NAME_SOURCE_TITLE_ID_INDEX 0", header_lines)
            self.assertIn("#define NK_LAUNCH_NAME_SOURCE_GAME_NAME_INDEX 1", header_lines)
            committed = (
                REPO_ROOT / "src" / "core" / "generated" / "nk_title_catalog.h"
            ).read_text(encoding="utf-8")
            self.assertIn("#define NK_LAUNCH_NAME_SOURCE_GAME_NAME_INDEX 0", committed)
            self.assertNotIn("#define NK_LAUNCH_NAME_SOURCE_TITLE_ID_INDEX 0", committed)

            # Fail-closed shape: duplicated or non-macro-safe sources are
            # refused at generation time instead of emitting a broken bind.
            for bad in (("game_name", "game_name"), ("Game Name", "title_id")):
                tcg.NAME_SOURCES = bad
                with self.assertRaises(ValueError):
                    tcg._launch_contract_header_lines()
            tcg.NAME_SOURCES = reversed_sources

            # 2. Regenerate the FULL catalog under the reversed contract and
            # compile native against it (generated dir first on the include
            # path): nk_launch.c must follow the new order, not the old one.
            policy = publication_policy.load_policy(
                REPO_ROOT / "assets" / "public_source_profile.json"
            )
            manifest_files, titles = tcg.collect_public_manifests(
                REPO_ROOT / "assets" / "titles", policy=policy
            )
            digest = tcg.compute_manifest_digest(manifest_files)
            # Quote-includes resolve next to the including file first
            # ("generated/nk_title_catalog.h"), so -I ordering cannot
            # redirect the contract: the mutated header must sit beside a
            # copy of the sources that consume it.
            src_dir = self.temp_dir / "reordered_src"
            (src_dir / "generated").mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO_ROOT / "src" / "core" / "nk_launch.c", src_dir / "nk_launch.c")
            shutil.copy2(REPO_ROOT / "src" / "core" / "nk_launch.h", src_dir / "nk_launch.h")
            # nk_launch.c includes nk_title_manifest.h, which quote-includes the
            # catalog header; copy it too so the mutated header is the one seen.
            shutil.copy2(REPO_ROOT / "src" / "core" / "nk_title_manifest.c", src_dir / "nk_title_manifest.c")
            shutil.copy2(REPO_ROOT / "src" / "core" / "nk_title_manifest.h", src_dir / "nk_title_manifest.h")
            header_content = tcg.generate_header(digest, titles)
            source_content = tcg.generate_source(digest, titles)
            (src_dir / "generated" / "nk_title_catalog.h").write_text(
                header_content, encoding="utf-8", newline=""
            )
            (src_dir / "generated" / "nk_title_catalog.c").write_text(
                source_content, encoding="utf-8", newline=""
            )
            self.assertIn(
                "#define NK_LAUNCH_NAME_SOURCE_TITLE_ID_INDEX 0", header_content
            )

            harness_c = src_dir / "reorder_harness.c"
            harness_c.write_text(self.HARNESS_C, encoding="utf-8", newline="\n")
            out = self.temp_dir / (
                "reorder_harness.exe" if sys.platform == "win32" else "reorder_harness"
            )
            platform_src = (
                REPO_ROOT / "src" / "core" / "nk_platform_win32.c"
                if sys.platform == "win32"
                else REPO_ROOT / "src" / "core" / "nk_platform_posix.c"
            )
            cc = [
                self.gcc, "-std=c99", "-Wall", "-Wextra",
                "-I", str(src_dir),
                "-I", str(REPO_ROOT / "src" / "core"),
                "-I", str(REPO_ROOT / "src" / "core" / "generated"),
                str(harness_c),
                str(REPO_ROOT / "src" / "core" / "nk_iso.c"),
                str(REPO_ROOT / "src" / "core" / "nk_library.c"),
                str(src_dir / "nk_launch.c"),
                str(src_dir / "nk_title_manifest.c"),
                str(REPO_ROOT / "src" / "core" / "nk_json.c"),
                str(src_dir / "generated" / "nk_title_catalog.c"),
                str(platform_src),
                *(["-lshell32", "-lole32", "-luuid"] if sys.platform == "win32" else []),
                "-o", str(out),
            ]
            res = subprocess.run(cc, capture_output=True, text=True)
            self.assertEqual(
                res.returncode, 0,
                f"reordered-contract native build failed: {res.stderr}",
            )
            run = subprocess.run(
                [str(out), str(fixture_root), self.TITLE2_DISC, self.TITLE2, str(self.iso)],
                capture_output=True, text=True,
            )
            self.assertEqual(run.returncode, 0, run.stderr)
            native = self._parse_native(run.stdout)
            self.assertEqual(native.get("RESULT"), "0", native.get("ERROR"))

            # 3. Python under the same reversed contract.
            cmd_py, _ = self._python_plan(fixture_root)

            # Both planners flipped TOGETHER: native did not stay silently on
            # the old game_name-first order.
            self.assertTrue(
                self._same_path(native["EXE"], str(id_exe)),
                f"reordered contract not honored natively: {native.get('EXE')}",
            )
            self.assertFalse(self._same_path(native["EXE"], str(game_exe)))
            self.assertTrue(self._same_path(cmd_py[0], str(id_exe)))
            self.assertTrue(self._same_path(cmd_py[0], native["EXE"]))
            self.assertTrue(self._same_path(cmd_py[2], native["IMAGE"]))
        finally:
            tcg.NAME_SOURCES = original_sources
            nk_launcher.NAME_SOURCES = original_planner


class GenericLauncherReintroductionGateTests(unittest.TestCase):
    """Source-shape and mutation gates against retail fallback reintroduction."""

    # Name-source ordering gate: native may bind name sources ONLY through
    # the generated index macros. A positional names[0]/names[1] assignment
    # would silently pin the order regardless of NAME_SOURCES -- exactly the
    # hole this suite exists to keep closed.
    def test_native_name_sources_bound_only_via_generated_indices(self) -> None:
        import title_catalog_codegen as tcg

        src = (REPO_ROOT / "src" / "core" / "nk_launch.c").read_text(encoding="utf-8")
        self.assertIn("launch_bind_name_sources", src)
        self.assertIn("NK_LAUNCH_NAME_SOURCE_GAME_NAME_INDEX", src)
        self.assertIn("NK_LAUNCH_NAME_SOURCE_TITLE_ID_INDEX", src)
        for positional in ("names[0]", "names[1]"):
            self.assertNotIn(
                positional, src,
                f"{positional} hard-codes name-source order; bind with "
                "launch_bind_name_sources (generated indices) instead",
            )
        header = (
            REPO_ROOT / "src" / "core" / "generated" / "nk_title_catalog.h"
        ).read_text(encoding="utf-8")
        # The committed header must agree with the live planner projection,
        # macro for macro -- reorder or substitution fails here too.
        for line in tcg._launch_contract_header_lines():
            if line.startswith("#define NK_LAUNCH_NAME_SOURCE"):
                self.assertIn(
                    line, header,
                    "committed generated header drifted from NAME_SOURCES projection",
                )

    TITLE2 = "pspdev-phase5-v1"
    TITLE2_DISC = "TEST00005"
    TITLE2_GAME = "pspdev-phase5"
    PY_EXE_MARKER = '    "build/{name}/{name}.exe",'
    # Unique anchor inside find_candidate_executable: right after `sep` is
    # bound, with root/out_path/max_len in scope, before the contract loop.
    C_SEP_MARKER = "    sep = nk_platform_path_separator();"
    NATIVE_PROBE_MUTANT = (
        "    /* mutant: reintroduced retail-title first probe */\n"
        "    {\n"
        "        char mc[NK_MAX_PATH];\n"
        "        snprintf(mc, sizeof(mc), \"%s%cbuild%chst%chst.exe\", root, sep, sep, sep);\n"
        "        if (nk_platform_file_exists(mc)) {\n"
        "            snprintf(out_path, max_len, \"%s\", mc);\n"
        "            return true;\n"
        "        }\n"
        "        snprintf(mc, sizeof(mc), \"%s%cbuild%chst%chst\", root, sep, sep, sep);\n"
        "        if (nk_platform_file_exists(mc)) {\n"
        "            snprintf(out_path, max_len, \"%s\", mc);\n"
        "            return true;\n"
        "        }\n"
        "    }\n"
    )

    def setUp(self) -> None:
        self.temp_dir = Path(tempfile.mkdtemp(prefix="nk_launch_gate_"))
        self.iso = self.temp_dir / "source.iso"
        self.iso.write_bytes(b"iso")

    def tearDown(self) -> None:
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    # Gate 1: no retail identity default or retail path probe may exist in the
    # generic launcher sources.
    def test_generic_launcher_sources_carry_no_retail_fallbacks(self) -> None:
        for rel in GENERIC_LAUNCHER_SOURCES:
            text = (REPO_ROOT / rel).read_text(encoding="utf-8")
            findings = _retail_fallback_findings(text)
            self.assertEqual(
                findings, [],
                f"{rel} reintroduces retail-title generic fallbacks: {findings}",
            )

    # Gate 2: the shape check itself blocks a reintroduced Python default.
    def test_source_shape_gate_blocks_reintroduced_python_default(self) -> None:
        src = (REPO_ROOT / "tools" / "nk_core" / "launcher.py").read_text(encoding="utf-8")
        marker = 'title_id = manifest.get("title_id")'
        self.assertIn(marker, src, "launcher identity parsing must stay default-free")
        mutant = src.replace(
            marker,
            'title_id = manifest.get("title_id", "hst")\n'
            '        disc_id = manifest.get("disc_id", "UCUS98701")',
            1,
        )
        self.assertNotEqual(mutant, src)
        findings = _retail_fallback_findings(mutant)
        self.assertIn("UCUS98701", findings)
        self.assertIn('"hst"', findings)

    # Gate 3: the shape check blocks a reintroduced native retail probe.
    def test_source_shape_gate_blocks_reintroduced_native_probe(self) -> None:
        src = (REPO_ROOT / "src" / "core" / "nk_launch.c").read_text(encoding="utf-8")
        self.assertIn(self.C_SEP_MARKER, src)
        mutant = src.replace(
            self.C_SEP_MARKER, self.C_SEP_MARKER + "\n" + self.NATIVE_PROBE_MUTANT, 1
        )
        self.assertNotEqual(mutant, src)
        findings = _retail_fallback_findings(mutant)
        self.assertIn("%chst", findings)

    # Gate 4 (behavioral): an HST-first candidate smuggled into the Python
    # contract is executed and caught by the hostile contract: the plan would
    # select the stale retail build, which the hostile test rejects.
    def test_python_hst_first_candidate_mutant_is_killed(self) -> None:
        src = (REPO_ROOT / "tools" / "nk_core" / "launcher.py").read_text(encoding="utf-8")
        self.assertIn(self.PY_EXE_MARKER, src)
        mutant_src = src.replace(
            self.PY_EXE_MARKER,
            '    "build/hst/hst.exe",\n    "build/hst/hst",\n' + self.PY_EXE_MARKER,
            1,
        )
        self.assertNotEqual(mutant_src, src)
        self.assertIn("build/hst", _retail_fallback_findings(mutant_src))

        root = self.temp_dir / "ws"
        _write_stale_other_title_artifacts(root)
        game_dir = root / "TEST00005"
        game_dir.mkdir(parents=True, exist_ok=True)
        (game_dir / "manifest.json").write_text(
            json.dumps(
                {
                    "title_id": self.TITLE2,
                    "disc_id": self.TITLE2_DISC,
                    "iso_path": str(self.iso),
                }
            ),
            encoding="utf-8",
        )

        # The real planner fails closed for the missing second-title runtime.
        with self.assertRaises(RuntimeLaunchError) as cm:
            RuntimeLauncher(repo_root=root).build_launch_plan(game_dir)
        self.assertIn("Runtime binary not found", str(cm.exception))

        # The mutant executes and wrongly selects the stale retail build: that
        # is exactly the outcome the hostile contract refuses, so the hostile
        # test kills this mutant (and the shape gate flags its text too).
        mutant = _load_python_module_from_source("nk_core.launcher_mutant", mutant_src)
        mutant_cmd, _ = mutant.RuntimeLauncher(repo_root=root).build_launch_plan(game_dir)
        selected = os.path.normpath(mutant_cmd[0]).replace("\\", "/")
        self.assertIn("build/hst/", selected)

    # Gate 5 (behavioral, compile + execute): an HST-first candidate smuggled
    # into the native launcher is compiled, run against the hostile fixture,
    # and wrongly resolves the stale retail build -- which the native hostile
    # contract refuses, so the gate kills the mutant.
    def test_native_hst_first_candidate_mutant_is_killed(self) -> None:
        gcc = shutil.which("gcc")
        if not gcc:
            self.skipTest("gcc not available for the native mutation proof")

        src = (REPO_ROOT / "src" / "core" / "nk_launch.c").read_text(encoding="utf-8")
        self.assertIn(self.C_SEP_MARKER, src)
        mutant_src = src.replace(
            self.C_SEP_MARKER, self.C_SEP_MARKER + "\n" + self.NATIVE_PROBE_MUTANT, 1
        )
        self.assertNotEqual(mutant_src, src)
        self.assertIn("%chst", _retail_fallback_findings(mutant_src))

        mutant_c = self.temp_dir / "mutant_launch.c"
        mutant_c.write_text(mutant_src, encoding="utf-8", newline="\n")

        harness_c = self.temp_dir / "mutant_harness.c"
        harness_c.write_text(
            GenericLauncherParityTests.HARNESS_C, encoding="utf-8", newline="\n"
        )
        out = self.temp_dir / ("mutant.exe" if sys.platform == "win32" else "mutant")
        core_srcs = [
            REPO_ROOT / "src" / "core" / "nk_iso.c",
            REPO_ROOT / "src" / "core" / "nk_library.c",
            mutant_c,
            REPO_ROOT / "src" / "core" / "nk_title_manifest.c",
            REPO_ROOT / "src" / "core" / "nk_json.c",
            REPO_ROOT / "src" / "core" / "generated" / "nk_title_catalog.c",
        ]
        if sys.platform == "win32":
            core_srcs.append(REPO_ROOT / "src" / "core" / "nk_platform_win32.c")
        else:
            core_srcs.append(REPO_ROOT / "src" / "core" / "nk_platform_posix.c")
        cmd = [
            gcc, "-std=c99", "-Wall", "-Wextra",
            "-I", str(REPO_ROOT / "src" / "core"),
            "-I", str(REPO_ROOT / "src" / "core" / "generated"),
            str(harness_c),
        ] + [str(s) for s in core_srcs] + (
            ["-lshell32", "-lole32", "-luuid"] if sys.platform == "win32" else []
        ) + ["-o", str(out)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"mutant compile failed: {res.stderr}")

        root = self.temp_dir / "ws"
        _write_stale_other_title_artifacts(root)
        run = subprocess.run(
            [str(out), str(root), self.TITLE2_DISC, self.TITLE2, str(self.iso)],
            capture_output=True, text=True,
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        parsed = GenericLauncherParityTests._parse_native(run.stdout)
        # The mutant compiled, executed, and wrongly resolved the stale retail
        # build with availability true -- the outcomes the native hostile
        # contract refuses, so the hostile gate kills this mutant.
        self.assertEqual(parsed.get("RESULT"), "0", parsed.get("ERROR"))
        selected = os.path.normpath(parsed.get("EXE", "")).replace("\\", "/")
        self.assertIn("build/hst/", selected)
        self.assertEqual(parsed.get("AVAILABLE"), "1")

class BoundedLibraryJsonTests(unittest.TestCase):
    """#319: the externally supplied library.json must fail closed and bounded.

    Every malformed case targets the production GameLibrary.load entry point
    (or the production nk_cli library route) and expects a named controlled
    ValueError diagnosis -- never AttributeError, TypeError, RecursionError,
    UnicodeDecodeError, or a silent acceptance.
    """

    def setUp(self) -> None:
        self.temp_dir = Path(tempfile.mkdtemp(prefix="test_library_bounded_"))
        self.addCleanup(shutil.rmtree, self.temp_dir, True)

    def _write(self, name: str, payload) -> Path:
        path = self.temp_dir / name
        if isinstance(payload, bytes):
            path.write_bytes(payload)
        else:
            path.write_text(payload, encoding="utf-8")
        return path

    @staticmethod
    def _record(**overrides) -> str:
        fields = {
            '"disc_id"': '"ULES00123"',
            '"title_name"': '"Synthetic Title"',
            '"iso_path"': '"synthetic.iso"',
        }
        for key, value in overrides.items():
            fields[f'"{key}"'] = value
        body = ", ".join(f"{key}: {value}" for key, value in fields.items())
        return "{" + body + "}"

    def _library_text(self, games_json: str) -> str:
        return '{"schema_version": 1, "updated_at": 1.5, "games": [' + games_json + "]}"

    def test_valid_library_round_trip_preserves_all_fields(self) -> None:
        path = self.temp_dir / "library.json"
        lib = GameLibrary(storage_file=path)
        lib.add_or_update_game(LibraryGameRecord(
            disc_id="ULES00123",
            title_name="Synthetic Title",
            iso_path="synthetic.iso",
            prepared_root="prepared",
            is_prepared=True,
            assets_staged=True,
            extracted_asset_count=7,
            extracted_audio_count=5,
            extracted_visual_count=3,
            extracted_layout_count=2,
            disc_version="1.02",
            last_played=1234.5,
            play_count=9,
            settings_override={"resolution_scale": 4},
        ))
        lib.save()
        loaded = GameLibrary.load(path)
        record = loaded.get_game("ules00123")
        self.assertIsNotNone(record)
        self.assertEqual(record.title_name, "Synthetic Title")
        self.assertEqual(record.play_count, 9)
        self.assertEqual(record.settings_override, {"resolution_scale": 4})
        self.assertEqual(record.last_played, 1234.5)

    def test_library_exactly_at_byte_limit_is_accepted(self) -> None:
        limit = package_cache.MAX_CACHE_JSON_BYTES
        prefix = '{"schema_version": 1, "updated_at": 1.5, "games": [{"disc_id": "ULES00123", "settings_override": {"pad": "'
        suffix = '"}}]}'
        text = prefix + "A" * (limit - len(prefix) - len(suffix)) + suffix
        self.assertEqual(len(text.encode("utf-8")), limit)
        path = self._write("at_limit.json", text)
        loaded = GameLibrary.load(path)
        self.assertEqual(loaded.count(), 1)
        self.assertIsNotNone(loaded.get_game("ules00123"))

    def test_large_fully_populated_library_round_trip(self) -> None:
        path = self.temp_dir / "large_library.json"
        lib = GameLibrary(storage_file=path)
        for index in range(1200):
            lib.add_or_update_game(LibraryGameRecord(
                disc_id=f"SYNTH{index:05d}", title_name=f"Synthetic {index}",
                iso_path="synthetic.iso", settings_override={"scale": 2},
            ))
        lib.save()
        self.assertLess(path.stat().st_size, package_cache.MAX_CACHE_JSON_BYTES)
        loaded = GameLibrary.load(path)
        self.assertEqual(loaded.count(), 1200)
        self.assertEqual(loaded.get_game("SYNTH01199").to_dict(),
                         lib.get_game("SYNTH01199").to_dict())

    def test_save_rejects_unreadable_output_without_replacing_library(self) -> None:
        path = self.temp_dir / "preserved_library.json"
        lib = GameLibrary(storage_file=path)
        lib.add_or_update_game(LibraryGameRecord(
            disc_id="SYNTH00001", title_name="Synthetic", iso_path="synthetic.iso",
        ))
        lib.save()
        before = path.read_bytes()
        lib.get_game("SYNTH00001").settings_override = {
            "padding": "x" * package_cache.MAX_CACHE_JSON_BYTES,
        }
        with self.assertRaisesRegex(ValueError, "byte.*limit"):
            lib.save()
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual([item.name for item in self.temp_dir.iterdir()], [path.name])
        self.assertEqual(GameLibrary.load(path).count(), 1)

    def test_library_target_with_tmp_suffix_is_preserved(self) -> None:
        path = self.temp_dir / "library.tmp"
        lib = GameLibrary(storage_file=path)
        lib.add_or_update_game(LibraryGameRecord(
            disc_id="SYNTH00001", title_name="Synthetic", iso_path="synthetic.iso",
        ))
        lib.save()
        self.assertEqual(GameLibrary.load(path).count(), 1)
        self.assertFalse(path.with_name(path.name + ".tmp").exists())

    def test_library_one_byte_over_limit_is_rejected(self) -> None:
        limit = package_cache.MAX_CACHE_JSON_BYTES
        prefix = '{"schema_version": 1, "updated_at": 1.5, "games": [{"disc_id": "ULES00123", "settings_override": {"pad": "'
        suffix = '"}}]}'
        text = prefix + "A" * (limit + 1 - len(prefix) - len(suffix)) + suffix
        self.assertEqual(len(text.encode("utf-8")), limit + 1)
        path = self._write("over_limit.json", text)
        with self.assertRaisesRegex(ValueError, "byte"):
            GameLibrary.load(path)

    def test_library_excessive_nesting_is_rejected(self) -> None:
        depth = 100_000  # stays under the byte ceiling so the depth gate fires
        text = (
            '{"schema_version": 1, "games": [{"disc_id": "ULES00123", "n": '
            + '{"n":' * depth + "1" + "}" * depth + "}]}")
        path = self._write("deep.json", text)
        with self.assertRaisesRegex(ValueError, "nesting exceeds 32"):
            GameLibrary.load(path)

    def test_library_excessive_record_count_is_rejected(self) -> None:
        from nk_core.library import MAX_LIBRARY_GAMES
        games = ",".join("null" for _ in range(MAX_LIBRARY_GAMES + 1))
        path = self._write("many.json", '{"schema_version": 1, "games": [' + games + "]}")
        with self.assertRaisesRegex(ValueError, "over the 4096-record limit"):
            GameLibrary.load(path)

    def test_library_duplicate_keys_are_rejected(self) -> None:
        path = self._write("dup.json", '{"schema_version": 1, "games": [], "games": []}')
        with self.assertRaisesRegex(ValueError, "duplicate JSON field: games"):
            GameLibrary.load(path)

    def test_library_malformed_utf8_is_a_named_error(self) -> None:
        path = self._write("bad_utf8.json", b'{"schema_version": 1, "games": [], "note": "\xff\xfe"}')
        with self.assertRaisesRegex(ValueError, "not valid UTF-8"):
            GameLibrary.load(path)

    def test_library_wrong_top_level_type_is_rejected(self) -> None:
        path = self._write("wrong_top.json", "[1, 2, 3]")
        with self.assertRaisesRegex(ValueError, "library root must be a JSON object"):
            GameLibrary.load(path)

    def test_library_games_null_record_is_a_controlled_error(self) -> None:
        path = self._write("null_game.json", '{"schema_version": 1, "games": [null]}')
        with self.assertRaisesRegex(ValueError, "game record 0 is invalid"):
            GameLibrary.load(path)

    def test_library_games_scalar_and_list_records_are_controlled_errors(self) -> None:
        for name, payload in (
            ("scalar.json", self._library_text("123")),
            ("list.json", self._library_text("[]")),
            ("string.json", self._library_text('"game"')),
        ):
            with self.subTest(payload=payload):
                path = self._write(name, payload)
                with self.assertRaisesRegex(ValueError, "game record 0 is invalid"):
                    GameLibrary.load(path)

    def test_library_record_field_substitutions_are_controlled_errors(self) -> None:
        cases = {
            "disc_id null": self._record(**{"disc_id": "null"}),
            "disc_id number": self._record(**{"disc_id": "123"}),
            "disc_id list": self._record(**{"disc_id": "[]"}),
            "title_name null": self._record(**{"title_name": "null"}),
            "iso_path number": self._record(**{"iso_path": "12.5"}),
            "is_prepared string": self._record(**{"is_prepared": '"yes"'}),
            "play_count negative": self._record(**{"play_count": "-1"}),
            "play_count boolean": self._record(**{"play_count": "true"}),
            "play_count float": self._record(**{"play_count": "1.5"}),
            "last_played string": self._record(**{"last_played": '"soon"'}),
            "settings_override list": self._record(**{"settings_override": "[]"}),
        }
        for label, record in cases.items():
            with self.subTest(case=label):
                path = self._write(
                    "record.json", '{"schema_version": 1, "games": [' + record + "]}"
                )
                with self.assertRaises(ValueError):
                    GameLibrary.load(path)

    def test_library_unknown_record_and_root_fields_are_rejected(self) -> None:
        path = self._write(
            "unknown_record.json",
            '{"schema_version": 1, "games": [{"disc_id": "ULES00123", "wat": 1}]}',
        )
        with self.assertRaisesRegex(ValueError, "unsupported fields: wat"):
            GameLibrary.load(path)
        path = self._write(
            "unknown_root.json", '{"schema_version": 1, "games": [], "extra": true}'
        )
        with self.assertRaisesRegex(ValueError, "unsupported fields: extra"):
            GameLibrary.load(path)

    def test_library_duplicate_disc_id_records_are_rejected(self) -> None:
        record = self._record()
        path = self._write(
            "repeat.json", '{"schema_version": 1, "games": [' + record + ", " + record + "]}"
        )
        with self.assertRaisesRegex(ValueError, "repeats disc_id ULES00123"):
            GameLibrary.load(path)

    def test_library_missing_games_field_and_bad_schema_are_rejected(self) -> None:
        path = self._write("no_games.json", '{"schema_version": 1}')
        with self.assertRaisesRegex(ValueError, "missing the games field"):
            GameLibrary.load(path)
        path = self._write("bad_schema.json", '{"schema_version": "1", "games": []}')
        with self.assertRaisesRegex(ValueError, "unsupported library schema_version"):
            GameLibrary.load(path)
        path = self._write("newer_schema.json", '{"schema_version": 2, "games": []}')
        with self.assertRaisesRegex(ValueError, "unsupported library schema_version"):
            GameLibrary.load(path)

    def test_library_diagnoses_reject_hostile_content_without_echoing_it(self) -> None:
        """#319: every attacker-reachable rejection stays named AND bounded.

        library.json is attacker-supplied, so each untrusted value that a
        diagnosis would otherwise quote -- an unknown root field, an unknown
        record field, a non-integer schema_version, a repeated disc_id -- is
        echoed only as a short prefix. Each case still fails closed with a
        ValueError naming the condition; only the echoed detail is bounded.
        """
        bulk = "A" * 400_000
        cases = {
            "unknown_root": (
                '{"schema_version": 1, "updated_at": null, "games": [], "%s": 1}' % bulk,
                "unsupported fields",
            ),
            "unknown_record": (
                '{"schema_version": 1, "games": [{"disc_id": "ULES00123", "%s": 1}]}' % bulk,
                "unsupported fields",
            ),
            "schema_echo": (
                '{"schema_version": "%s", "games": []}' % bulk,
                "unsupported library schema_version",
            ),
            "duplicate_disc": (
                '{"schema_version": 1, "games": [{"disc_id": "%s"}, {"disc_id": "%s"}]}'
                % (bulk, bulk),
                "repeats disc_id",
            ),
        }
        for name, (text, expected) in cases.items():
            path = self._write(name + ".json", text)
            with self.assertRaises(ValueError) as caught:
                GameLibrary.load(path)
            message = str(caught.exception)
            self.assertIn(expected, message, name)
            self.assertNotIn(bulk, message, name)
            # A fixed prefix, plus the resolved local path and the index.
            self.assertLess(len(message), 512, name)

        # Many unknown fields are sampled, not concatenated.
        many = '{"schema_version": 1, "games": []' + "".join(
            ', "f%03d": 1' % i for i in range(500)
        ) + "}"
        path = self._write("many_fields.json", many)
        with self.assertRaises(ValueError) as caught:
            GameLibrary.load(path)
        self.assertIn("(+492 more)", str(caught.exception))
        self.assertLess(len(str(caught.exception)), 1024)

    def test_nk_cli_library_route_enforces_the_same_ceilings(self) -> None:
        user_root = self.temp_dir / "user"
        user_root.mkdir()
        limit = package_cache.MAX_CACHE_JSON_BYTES
        prefix = '{"schema_version": 1, "games": [{"disc_id": "ULES00123", "pad": "'
        suffix = '"}]}'
        (user_root / "library.json").write_text(
            prefix + "A" * (limit + 1 - len(prefix) - len(suffix)) + suffix, encoding="utf-8"
        )
        with self.assertRaisesRegex(nk_cli.PackageBuildError, "not valid bounded JSON"):
            nk_cli._load_library_entry(user_root, "ULES00123")

        valid = (
            '{"schema_version": 1, "games": [{"disc_id": "ULES00123", '
            '"title_id": "NPUG80318", "iso_path": "synthetic.iso", '
            '"selected_executable": "BOOT.BIN", "is_experimental": false}]}'
        )
        (user_root / "library.json").write_text(valid, encoding="utf-8")
        entry = nk_cli._load_library_entry(user_root, "ULES00123")
        self.assertEqual(entry["title_id"], "NPUG80318")
        self.assertEqual(entry["iso_path"], "synthetic.iso")

    def _cli_library(self, count: int) -> Path:
        path = self.temp_dir / "library.json"
        games = [{"disc_id": f"TEST{i:05d}", "title_id": "synthetic",
                  "iso_path": "synthetic.iso", "selected_executable": "BOOT.BIN",
                  "is_experimental": False} for i in range(count)]
        path.write_text(json.dumps({"schema_version": 1, "games": games}), encoding="utf-8")
        return path

    def test_cli_library_at_record_limit_is_readable(self) -> None:
        self._cli_library(nk_cli.MAX_LIBRARY_GAMES)
        disc_id = f"TEST{nk_cli.MAX_LIBRARY_GAMES - 1:05d}"
        nk_cli._write_bringup_library(
            self.temp_dir, Path("replacement.iso"), SimpleNamespace(disc_id=disc_id),
            "synthetic", "BOOT.BIN", False,
        )
        entry = nk_cli._load_library_entry(self.temp_dir, disc_id)
        self.assertEqual(entry["title_id"], "synthetic")
        self.assertEqual(entry["iso_path"], "replacement.iso")

    def test_cli_library_append_over_limit_preserves_existing_file(self) -> None:
        path = self._cli_library(2)
        before = path.read_bytes()
        with patch.object(nk_cli, "MAX_LIBRARY_GAMES", 2):
            with self.assertRaisesRegex(nk_cli.PackageBuildError, "record limit"):
                nk_cli._write_bringup_library(
                    self.temp_dir, Path("synthetic.iso"),
                    SimpleNamespace(disc_id="TEST99999"), "synthetic", "BOOT.BIN", False,
                )
        self.assertEqual(path.read_bytes(), before)

    def test_cli_library_oversized_write_preserves_existing_file(self) -> None:
        path = self._cli_library(1)
        before = path.read_bytes()
        with self.assertRaisesRegex(nk_cli.PackageBuildError, "byte limit"):
            nk_cli._write_bringup_library(
                self.temp_dir, Path("synthetic.iso"),
                SimpleNamespace(disc_id="TEST99999"),
                "A" * package_cache.MAX_CACHE_JSON_BYTES, "BOOT.BIN", False,
            )
        self.assertEqual(path.read_bytes(), before)

    def test_cli_library_writer_rejects_boolean_schema(self) -> None:
        path = self._cli_library(1)
        path.write_text(path.read_text().replace('"schema_version": 1', '"schema_version": true'),
                        encoding="utf-8")
        before = path.read_bytes()
        with self.assertRaisesRegex(nk_cli.PackageBuildError, "unsupported schema"):
            nk_cli._write_bringup_library(
                self.temp_dir, Path("synthetic.iso"),
                SimpleNamespace(disc_id="TEST99999"), "synthetic", "BOOT.BIN", False,
            )
        self.assertEqual(path.read_bytes(), before)

    def test_library_nonfinite_timestamps_are_controlled_errors(self) -> None:
        for field in ("updated_at", "last_played"):
            with self.subTest(field=field):
                if field == "updated_at":
                    text = '{"schema_version":1,"updated_at":NaN,"games":[]}'
                else:
                    text = self._library_text(self._record(last_played="1e400"))
                path = self._write("nonfinite.json", text)
                with self.assertRaisesRegex(ValueError, "library JSON.*finite"):
                    GameLibrary.load(path)

    def test_overlapping_library_saves_use_independent_temporary_files(self) -> None:
        path = self.temp_dir / "library.json"
        first = GameLibrary(storage_file=path)
        second = GameLibrary(storage_file=path)
        first.add_or_update_game(LibraryGameRecord("TEST00001", "First", "first.iso"))
        second.add_or_update_game(LibraryGameRecord("TEST00002", "Second", "second.iso"))
        real_load = GameLibrary.load
        interleaved = False

        def validate_with_another_writer(temporary):
            nonlocal interleaved
            if not interleaved:
                interleaved = True
                # The second production save completes between the first one's
                # temporary write and validation; no threads or timing guesses.
                second.save()
            return real_load(temporary)

        with patch.object(GameLibrary, "load", side_effect=validate_with_another_writer):
            first.save()
        loaded = GameLibrary.load(path)
        self.assertEqual([game.disc_id for game in loaded.list_games()], ["TEST00001"])
        self.assertEqual(sorted(item.name for item in self.temp_dir.iterdir()), ["library.json"])

    def test_library_save_preserves_another_writers_temporary_file(self) -> None:
        path = self.temp_dir / "library.json"
        other_temporary = self.temp_dir / "library.json.tmp"
        other_temporary.write_bytes(b"another writer's checkpoint")
        lib = GameLibrary(storage_file=path)
        lib.add_or_update_game(LibraryGameRecord("TEST00001", "Synthetic", "synthetic.iso"))
        lib.save()
        self.assertEqual(other_temporary.read_bytes(), b"another writer's checkpoint")
        self.assertEqual(GameLibrary.load(path).count(), 1)


if __name__ == "__main__":
    unittest.main()
