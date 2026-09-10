# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Unit tests for the portable nk_core preparation & runtime library."""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

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
    """Create a minimal valid ISO9660 image with PVD and disc ID signature."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        # Pad sectors 0-15 (16 * 2048 = 32768 bytes)
        f.write(b"\0" * (16 * 2048))
        # Sector 16: Primary Volume Descriptor
        pvd = bytearray(2048)
        pvd[0:7] = b"\x01CD001\x01"
        vol_bytes = volume_id.encode("latin-1")[:32].ljust(32)
        pvd[40:72] = vol_bytes
        f.write(pvd)
        # Sector 17: a real PARAM.SFO, so identity comes from parsed structure
        # rather than from a raw byte match somewhere in the image.
        sfo = _build_param_sfo(disc_id)
        sec17 = bytearray(2048)
        sec17[0 : len(sfo)] = sfo
        f.write(sec17)
        # Pad up to 1.5 MiB to satisfy min size
        remaining = (1536 * 1024) - (18 * 2048)
        f.write(b"\0" * remaining)


class NkCoreTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = Path(tempfile.mkdtemp(prefix="test_nk_core_"))

    def tearDown(self) -> None:
        shutil.rmtree(self.temp_dir, ignore_errors=True)

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

        # Verify no staging directories were left behind
        staging_dirs = list(dest_root.glob(".staging_*"))
        self.assertEqual(len(staging_dirs), 0)

        # Verify progress event sequence
        stages = [e.stage.value for e in events]
        self.assertIn("INSPECTING_ISO", stages)
        self.assertIn("READY", stages)

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
            "title_id": "synthetic-v1",
            "disc_id": "TEST00001",
            "iso_path": str(mock_iso),
        }
        manifest_file.write_text(json.dumps(manifest_data), encoding="utf-8")

        # Create mock executable
        mock_exe = self.temp_dir / "hst.exe"
        mock_exe.write_bytes(b"MZfake")

        launcher = RuntimeLauncher(repo_root=self.temp_dir)
        cmd, env = launcher.build_launch_plan(
            game_dir,
            profile="Benchmark",
            fps_cap=60,
        )

        self.assertEqual(cmd[0], str(mock_exe))
        self.assertEqual(env["PSP_ISO"], str(mock_iso))
        self.assertEqual(env["SR_FPS_CAP"], "60")
        self.assertEqual(env["SR_GPU_GE"], "1")
        self.assertEqual(env["SR_DEBUG"], "0x20")

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
            "title_id": "synthetic-v1",
            "disc_id": "TEST00001",
            "iso_path": str(self.temp_dir / "nonexistent" / "moved.iso"),
        }
        manifest_file.write_text(json.dumps(manifest_data), encoding="utf-8")

        mock_exe = self.temp_dir / "hst.exe"
        mock_exe.write_bytes(b"MZfake")

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
            "title_id": "synthetic-v1",
            "disc_id": "TEST00001",
            "iso_path": str(self.temp_dir / "old_location" / "moved.iso"),
        }
        manifest_file.write_text(json.dumps(manifest_data), encoding="utf-8")

        mock_exe = self.temp_dir / "hst.exe"
        mock_exe.write_bytes(b"MZfake")

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

        # Verify launch plan preserves spaces
        mock_exe = self.temp_dir / "hst.exe"
        mock_exe.write_bytes(b"MZfake")
        launcher = RuntimeLauncher(repo_root=self.temp_dir)
        cmd, env = launcher.build_launch_plan(result.prepared_root)
        self.assertEqual(env["PSP_ISO"], str(spaced_iso))

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


if __name__ == "__main__":
    unittest.main()
