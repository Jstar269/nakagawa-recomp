# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for PSP firmware PGF system font provisioning, structural validation, and local cache contract."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from nk_core.fonts import (  # noqa: E402
    FontImportError,
    FontValidationError,
    get_font_cache_dir,
    import_fonts,
    inspect_font_cache,
    resolve_font_directory,
    validate_pgf_data,
    validate_pgf_file,
)


def make_synthetic_pgf(
    header_offset: int = 0,
    header_size: int = 392,
    magic: bytes = b"PGF0",
    revision: int = 2,
    version: int = 6,
    first_glyph: int = 0,
    last_glyph: int = 10,
    total_size: int = 392,
) -> bytes:
    """Create a minimal synthetic PGF-shaped buffer without real font bytes."""
    buf = bytearray(max(total_size, header_offset + header_size))
    struct.pack_into("<HH4sii", buf, header_offset, header_offset, header_size, magic, revision, version)
    if header_size >= 186:
        struct.pack_into("<HH", buf, header_offset + 182, first_glyph, last_glyph)
    return bytes(buf[:total_size])


class FontProvisioningTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp_dir = tempfile.TemporaryDirectory()
        self.temp_path = Path(self._temp_dir.name)

    def tearDown(self) -> None:
        self._temp_dir.cleanup()

    def test_synthetic_pgf_valid(self) -> None:
        """Minimal synthetic PGF passes structural validation and produces size/sha256."""
        pgf_bytes = make_synthetic_pgf()
        res = validate_pgf_data(pgf_bytes, "synthetic.pgf")
        self.assertEqual(res["size"], 392)
        self.assertEqual(res["sha256"], hashlib.sha256(pgf_bytes).hexdigest())

        # Test validate_pgf_file
        fpath = self.temp_path / "synthetic.pgf"
        fpath.write_bytes(pgf_bytes)
        fres = validate_pgf_file(fpath)
        self.assertEqual(fres["size"], 392)
        self.assertEqual(fres["sha256"], res["sha256"])

    def test_truncated_pgf_rejected(self) -> None:
        """Declared header size exceeding file size is rejected as truncated."""
        truncated_bytes = make_synthetic_pgf(total_size=100)  # declares 392 bytes, has 100
        with self.assertRaises(FontValidationError) as ctx:
            validate_pgf_data(truncated_bytes, "truncated.pgf")
        self.assertIn("truncated", str(ctx.exception).lower())
        self.assertIn("truncated.pgf", str(ctx.exception))

    def test_wrong_magic_pgf_rejected(self) -> None:
        """PGF file with invalid magic signature is rejected with diagnostic."""
        bad_magic_bytes = make_synthetic_pgf(magic=b"BAD0")
        with self.assertRaises(FontValidationError) as ctx:
            validate_pgf_data(bad_magic_bytes, "bad_magic.pgf")
        self.assertIn("invalid magic", str(ctx.exception).lower())
        self.assertIn("bad_magic.pgf", str(ctx.exception))

    def test_corrupt_glyph_bounds_rejected(self) -> None:
        """PGF file with first_glyph > last_glyph is rejected as corrupt."""
        corrupt_bytes = make_synthetic_pgf(first_glyph=50, last_glyph=10)
        with self.assertRaises(FontValidationError) as ctx:
            validate_pgf_data(corrupt_bytes, "corrupt.pgf")
        self.assertIn("corrupt", str(ctx.exception).lower())

    def test_import_success_with_manifest(self) -> None:
        """Importing valid font dumps copies files into user-data cache and writes manifest.json."""
        source_dir = self.temp_path / "firmware_dump"
        source_dir.mkdir()
        (source_dir / "jpn0.pgf").write_bytes(make_synthetic_pgf(first_glyph=0, last_glyph=20))
        (source_dir / "ltn0.pgf").write_bytes(make_synthetic_pgf(first_glyph=0, last_glyph=5))

        user_data = self.temp_path / "user_data"
        result = import_fonts(source_dir, user_data_root=user_data)

        self.assertEqual(result["status"], "OK")
        self.assertEqual(result["imported_count"], 2)
        cache_dir = get_font_cache_dir(user_data)
        self.assertTrue((cache_dir / "jpn0.pgf").is_file())
        self.assertTrue((cache_dir / "ltn0.pgf").is_file())

        manifest_path = cache_dir / "manifest.json"
        self.assertTrue(manifest_path.is_file())
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema_version"], 1)
        self.assertIn("import_time", manifest)
        self.assertIn("jpn0.pgf", manifest["files"])
        self.assertIn("ltn0.pgf", manifest["files"])
        self.assertEqual(manifest["files"]["jpn0.pgf"]["size"], 392)

    def test_reimport_idempotent(self) -> None:
        """Re-importing the same font files succeeds and preserves a consistent cache."""
        source_dir = self.temp_path / "firmware_dump"
        source_dir.mkdir()
        (source_dir / "jpn0.pgf").write_bytes(make_synthetic_pgf())

        user_data = self.temp_path / "user_data"
        res1 = import_fonts(source_dir, user_data_root=user_data)
        res2 = import_fonts(source_dir, user_data_root=user_data)
        self.assertEqual(res1["status"], "OK")
        self.assertEqual(res2["status"], "OK")
        self.assertEqual(res1["files"]["jpn0.pgf"]["sha256"], res2["files"]["jpn0.pgf"]["sha256"])

    def test_failed_import_keeps_old_cache(self) -> None:
        """A partially failed or corrupt import leaves previous cache completely intact."""
        # 1. Successful initial import
        valid_dump = self.temp_path / "valid_dump"
        valid_dump.mkdir()
        initial_bytes = make_synthetic_pgf()
        (valid_dump / "jpn0.pgf").write_bytes(initial_bytes)
        user_data = self.temp_path / "user_data"
        import_fonts(valid_dump, user_data_root=user_data)
        cache_dir = get_font_cache_dir(user_data)
        initial_manifest = (cache_dir / "manifest.json").read_text(encoding="utf-8")

        # 2. Second import attempt with a corrupt file
        corrupt_dump = self.temp_path / "corrupt_dump"
        corrupt_dump.mkdir()
        (corrupt_dump / "ltn0.pgf").write_bytes(make_synthetic_pgf(total_size=50))  # truncated

        with self.assertRaises(FontValidationError):
            import_fonts(corrupt_dump, user_data_root=user_data)

        # 3. Verify previous cache is unchanged
        current_manifest = (cache_dir / "manifest.json").read_text(encoding="utf-8")
        self.assertEqual(initial_manifest, current_manifest)
        self.assertEqual((cache_dir / "jpn0.pgf").read_bytes(), initial_bytes)
        self.assertFalse((cache_dir / "ltn0.pgf").exists())

        # 4. Import from empty folder raises FontImportError
        empty_dump = self.temp_path / "empty_dump"
        empty_dump.mkdir()
        with self.assertRaises(FontImportError):
            import_fonts(empty_dump, user_data_root=user_data)

    def test_preflight_missing_ok_invalid(self) -> None:
        """Preflight discovery reports MISSING, OK, and INVALID correctly."""
        user_data = self.temp_path / "user_data"
        cache_dir = get_font_cache_dir(user_data)

        # 1. MISSING
        status, msg = inspect_font_cache(user_data_root=user_data)
        self.assertEqual(status, "MISSING")
        self.assertIn("run fonts import <folder>", msg)
        self.assertIn("#300", msg)

        # 2. OK (after import)
        dump_dir = self.temp_path / "dump"
        dump_dir.mkdir()
        (dump_dir / "jpn0.pgf").write_bytes(make_synthetic_pgf())
        import_fonts(dump_dir, user_data_root=user_data)
        status, msg = inspect_font_cache(user_data_root=user_data)
        self.assertEqual(status, "OK")
        self.assertIn("available", msg)
        self.assertEqual(resolve_font_directory(user_data_root=user_data), cache_dir)

        # 3. INVALID: corrupt manifest JSON
        (cache_dir / "manifest.json").write_text("{invalid json", encoding="utf-8")
        status, msg = inspect_font_cache(user_data_root=user_data)
        self.assertEqual(status, "INVALID")
        self.assertIn("run fonts import <folder>", msg)

        # 4. INVALID: missing declared file on disk
        manifest_data = {
            "schema_version": 1,
            "import_time": "2026-09-24T00:00:00Z",
            "files": {
                "jpn0.pgf": {"size": 392, "sha256": "0" * 64}
            }
        }
        (cache_dir / "manifest.json").write_text(json.dumps(manifest_data), encoding="utf-8")
        (cache_dir / "jpn0.pgf").unlink()
        status, msg = inspect_font_cache(user_data_root=user_data)
        self.assertEqual(status, "INVALID")

        # 5. INVALID: file truncated on disk
        (cache_dir / "jpn0.pgf").write_bytes(b"short")
        status, msg = inspect_font_cache(user_data_root=user_data)
        self.assertEqual(status, "INVALID")

    def test_cli_fonts_import_command(self) -> None:
        """CLI `nk_cli.py fonts import <folder>` executes and returns JSON or text."""
        dump_dir = self.temp_path / "dump"
        dump_dir.mkdir()
        (dump_dir / "jpn0.pgf").write_bytes(make_synthetic_pgf())
        user_data = self.temp_path / "cli_user_data"

        res = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools" / "nk_cli.py"),
                "fonts",
                "import",
                str(dump_dir),
                "--user-data-root",
                str(user_data),
                "--json",
            ],
            capture_output=True,
            text=True,
            cwd=ROOT,
            check=False,
        )
        self.assertEqual(res.returncode, 0, res.stderr)
        data = json.loads(res.stdout)
        self.assertEqual(data["status"], "OK")
        self.assertIn("jpn0.pgf", data["files"])

    def test_cli_fonts_import_corrupt_fails(self) -> None:
        """CLI `nk_cli.py fonts import <folder>` fails with non-zero code on corrupt input."""
        dump_dir = self.temp_path / "bad_dump"
        dump_dir.mkdir()
        (dump_dir / "jpn0.pgf").write_bytes(b"not a valid font")
        user_data = self.temp_path / "cli_user_data"

        res = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools" / "nk_cli.py"),
                "fonts",
                "import",
                str(dump_dir),
                "--user-data-root",
                str(user_data),
            ],
            capture_output=True,
            text=True,
            cwd=ROOT,
            check=False,
        )
        self.assertEqual(res.returncode, 1)
        self.assertIn("Font import error", res.stderr)

    def test_public_candidate_policy_cannot_include_user_data_fonts(self) -> None:
        """Public source candidate and policy never include user-data fonts."""
        from tools import publication_policy

        policy = publication_policy.load_policy(ROOT / "assets" / "public_source_profile.json")
        for cand in (
            "fonts/v1/manifest.json",
            "fonts/v1/jpn0.pgf",
            "fonts/manifest.json",
        ):
            res = policy.resolve(cand)
            self.assertNotEqual(res.disposition, publication_policy.INCLUDED)
            self.assertNotIn(cand, policy.include_paths)

        for cand in (
            "font/jpn0.pgf",
            "font/ltn0.pgf",
        ):
            res = policy.resolve(cand)
            self.assertEqual(res.disposition, publication_policy.EXCLUDED)

    def test_consumer_documentation_contracts(self) -> None:
        """README and docs/SETUP.md document user font import, cache path, and issue references."""
        readme_text = (ROOT / "README.md").read_text(encoding="utf-8")
        setup_text = (ROOT / "docs" / "SETUP.md").read_text(encoding="utf-8")

        # README consumer section documents fonts import and issues #300 / #313
        self.assertIn("tools/nk_cli.py fonts import", readme_text)
        self.assertIn("#300", readme_text)
        self.assertIn("#313", readme_text)

        # docs/SETUP.md contains System fonts subsection with command, cache, and issue references
        self.assertIn("### System fonts", setup_text)
        self.assertIn("python tools/nk_cli.py fonts import <folder>", setup_text)
        self.assertIn("fonts/v1", setup_text)
        self.assertIn("manifest.json", setup_text)
        self.assertIn("SYSTEM_FONTS", setup_text)
        self.assertIn("#300", setup_text)
        self.assertIn("#313", setup_text)


if __name__ == "__main__":
    unittest.main()
