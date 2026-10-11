# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for the per-user PSP system-font import: PGF checks, slot classification, manifest v2,
per-slot status, removal, and the nk_cli commands. Every font is a synthetic image built with
tools/pgf_writer.py; no real font is used."""

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
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

from nk_core.fonts import (  # noqa: E402
    FontImportError,
    FontValidationError,
    get_font_cache_dir,
    import_fonts,
    inspect_font_cache,
    remove_imports,
    resolve_font_directory,
    slot_states,
    validate_pgf_data,
    validate_pgf_file,
)
from pgf_writer import Glyph, build_pgf  # noqa: E402

SLOT_FILE = {"japanese": "nkjpn.pgf", "latin": "nkltn.pgf", "korean": "nkkr.pgf"}


def _glyph(code: int) -> Glyph:
    return Glyph(code=code, width=2, height=2, samples=(1, 2, 3, 4), row_order=1)


def latin_pgf() -> bytes:
    return build_pgf([_glyph(0x41), _glyph(0x61)], font_name="Nakagawa Test Latin")


def japanese_pgf() -> bytes:
    return build_pgf([_glyph(0x41), _glyph(0x3042)], font_name="Nakagawa Test Japanese")


def korean_pgf() -> bytes:
    return build_pgf([_glyph(0x41), _glyph(0xAC00)], font_name="Nakagawa Test Korean")


def dense_pgf() -> bytes:
    return build_pgf([_glyph(code) for code in range(0x41, 0x5B)], font_name="Nakagawa Test Dense")


def sparse_pgf() -> bytes:
    return build_pgf([_glyph(0x41), _glyph(0x5A)], font_name="Nakagawa Test Sparse")


class FontProvisioningTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp_dir = tempfile.TemporaryDirectory()
        self.temp_path = Path(self._temp_dir.name)

    def tearDown(self) -> None:
        self._temp_dir.cleanup()

    def _folder(self, name: str, files: dict) -> Path:
        folder = self.temp_path / name
        folder.mkdir(parents=True, exist_ok=True)
        for file_name, data in files.items():
            (folder / file_name).write_bytes(data)
        return folder

    # Reader acceptance and refusals -------------------------------------------------------

    def test_dense_and_sparse_pgfs_are_accepted_with_their_facts(self) -> None:
        dense = validate_pgf_data(dense_pgf(), "dense.pgf")
        self.assertEqual(dense["glyph_count"], 26)
        self.assertEqual(dense["char_map_count"], 26)
        self.assertFalse(dense["sparse"])
        self.assertEqual(dense["sha256"], hashlib.sha256(dense_pgf()).hexdigest())
        sparse = validate_pgf_data(sparse_pgf(), "sparse.pgf")
        self.assertEqual(sparse["glyph_count"], 2)
        self.assertGreater(sparse["char_map_count"], sparse["glyph_count"])
        self.assertTrue(sparse["sparse"])

    def test_truncated_and_bad_magic_are_refused_by_name(self) -> None:
        with self.assertRaises(FontValidationError) as truncated:
            validate_pgf_data(dense_pgf()[:100], "truncated.pgf")
        self.assertIn("truncated", str(truncated.exception))
        self.assertIn("truncated.pgf", str(truncated.exception))
        bad = bytearray(dense_pgf())
        bad[4:8] = b"BAD0"
        with self.assertRaises(FontValidationError) as magic:
            validate_pgf_data(bytes(bad), "bad_magic.pgf")
        self.assertIn("invalid PGF magic", str(magic.exception))

    def test_first_after_last_revision_four_and_412_under_revision_two_are_refused(self) -> None:
        image = bytearray(dense_pgf())
        image[0xB6:0xB8] = struct.pack("<H", 0x5B)
        with self.assertRaisesRegex(FontValidationError, "corrupt glyph indices"):
            validate_pgf_data(bytes(image), "range.pgf")
        image = bytearray(dense_pgf())
        image[8:12] = struct.pack("<i", 4)
        with self.assertRaisesRegex(FontValidationError, "unsupported revision"):
            validate_pgf_data(bytes(image), "revision4.pgf")
        image = bytearray(dense_pgf())
        image[2:4] = struct.pack("<H", 412)
        with self.assertRaisesRegex(FontValidationError, "header size"):
            validate_pgf_data(bytes(image), "header412.pgf")

    def test_file_over_sixteen_mib_is_refused_unread(self) -> None:
        big = self.temp_path / "big.pgf"
        with big.open("wb") as handle:
            handle.truncate(16 * 1024 * 1024 + 1)
        with self.assertRaisesRegex(FontValidationError, "16 MiB"):
            validate_pgf_file(big)

    def test_slot_classification_follows_coverage_not_name(self) -> None:
        self.assertEqual(validate_pgf_data(latin_pgf())["slot"], "latin")
        self.assertEqual(validate_pgf_data(japanese_pgf())["slot"], "japanese")
        self.assertEqual(validate_pgf_data(korean_pgf())["slot"], "korean")
        digits = build_pgf([_glyph(0x30 + i) for i in range(10)], font_name="Nakagawa Test Digits")
        self.assertIsNone(validate_pgf_data(digits)["slot"])
        mixed = build_pgf([_glyph(0x3042), _glyph(0xAC00)], font_name="Nakagawa Test Mixed")
        self.assertIsNone(validate_pgf_data(mixed)["slot"])

    # Import, classify, manifest, status, and removal in a scratch folder --------------------

    def test_import_classifies_writes_manifest_v2_and_removes(self) -> None:
        source = self._folder("usb_folder", {
            "whatever_name.pgf": japanese_pgf(),
            "UPPER.PGF": latin_pgf(),
            "fontB.pgf": korean_pgf(),
            "readme.txt": b"not a font",
        })
        user_data = self.temp_path / "user_data"
        result = import_fonts(source, user_data_root=user_data)
        self.assertEqual(result["imported_count"], 3)
        self.assertEqual(result["status"], "OK")

        cache = get_font_cache_dir(user_data)
        self.assertEqual(cache, user_data / "fonts" / "v2")
        for name in SLOT_FILE.values():
            self.assertTrue((cache / name).is_file(), name)
        manifest = json.loads((cache / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema_version"], 2)
        self.assertIn("import_time", manifest)
        self.assertEqual(sorted(manifest["files"]), sorted(SLOT_FILE.values()))
        entry = manifest["files"]["nkjpn.pgf"]
        self.assertEqual(entry["slot"], "japanese")
        self.assertEqual(entry["source"], "user")
        self.assertEqual(entry["reader_version"], 1)
        self.assertEqual(entry["size"], len(japanese_pgf()))
        self.assertEqual(entry["sha256"], hashlib.sha256(japanese_pgf()).hexdigest())

        self.assertEqual(inspect_font_cache(user_data_root=user_data)[0], "OK")
        self.assertEqual(resolve_font_directory(user_data_root=user_data), cache)
        states = slot_states(user_data_root=user_data)
        self.assertEqual({slot: states[slot]["source"] for slot in states}, {
            "japanese": "user", "latin": "user", "korean": "user",
        })
        self.assertIn("imported from your PSP", states["japanese"]["detail"])

        # The originals are untouched by the import.
        self.assertEqual((source / "whatever_name.pgf").read_bytes(), japanese_pgf())

        removed = remove_imports(user_data_root=user_data)
        self.assertEqual(removed, 3)
        self.assertFalse((cache / "manifest.json").exists())
        for name in SLOT_FILE.values():
            self.assertFalse((cache / name).exists(), name)
        self.assertEqual(inspect_font_cache(user_data_root=user_data)[0], "MISSING")
        self.assertTrue((source / "whatever_name.pgf").is_file())

    def test_refused_files_are_reported_and_the_rest_import(self) -> None:
        source = self._folder("mixed", {
            "good.pgf": latin_pgf(),
            "broken.pgf": dense_pgf()[:100],
            "not-a-font.pgf": b"not a font at all",
        })
        result = import_fonts(source, user_data_root=self.temp_path / "ud")
        self.assertEqual(result["imported_count"], 1)
        details = {outcome["name"]: outcome["detail"] for outcome in result["outcomes"]}
        self.assertIn("truncated", details["broken.pgf"])
        self.assertTrue(details["not-a-font.pgf"].startswith("refused:"))
        self.assertIn("imported as the Latin font", details["good.pgf"])

    def test_duplicate_slot_needs_a_choice_and_the_choice_is_imported(self) -> None:
        source = self._folder("dupes", {"first.pgf": latin_pgf(), "second.pgf": latin_pgf()})
        user_data = self.temp_path / "ud"
        result = import_fonts(source, user_data_root=user_data)
        self.assertEqual(result["imported_count"], 0)
        self.assertNotIn("nkltn.pgf", result["files"])
        with self.assertRaises(FontImportError):
            import_fonts(source, user_data_root=user_data, choose={"latin": "missing.pgf"})
        result = import_fonts(source, user_data_root=user_data, choose={"latin": "second.pgf"})
        self.assertEqual(result["imported_count"], 1)
        self.assertEqual(result["slots"], {"latin": "nkltn.pgf"})

    def test_incremental_import_keeps_other_slots(self) -> None:
        user_data = self.temp_path / "ud"
        first = self._folder("first", {"a.pgf": japanese_pgf()})
        import_fonts(first, user_data_root=user_data)
        second = self._folder("second", {"b.pgf": korean_pgf()})
        import_fonts(second, user_data_root=user_data)
        states = slot_states(user_data_root=user_data)
        self.assertEqual(states["japanese"]["source"], "user")
        self.assertEqual(states["korean"]["source"], "user")
        self.assertEqual(states["latin"]["source"], "none")

    def test_project_font_serves_a_slot_with_no_user_font(self) -> None:
        user_data = self.temp_path / "ud"
        project = self.temp_path / "project"
        (project / "font").mkdir(parents=True)
        (project / "font" / "nkkr.pgf").write_bytes(korean_pgf())
        states = slot_states(user_data_root=user_data, project_root=project)
        self.assertEqual(states["korean"]["source"], "project")
        self.assertIn("project font", states["korean"]["detail"])
        self.assertEqual(states["latin"]["source"], "none")
        self.assertIn("missing", states["latin"]["detail"])

    def test_remove_names_one_slot(self) -> None:
        source = self._folder("all", {"a.pgf": japanese_pgf(), "b.pgf": latin_pgf()})
        user_data = self.temp_path / "ud"
        import_fonts(source, user_data_root=user_data)
        self.assertEqual(remove_imports(user_data_root=user_data, slots=["latin"]), 1)
        states = slot_states(user_data_root=user_data)
        self.assertEqual(states["japanese"]["source"], "user")
        self.assertEqual(states["latin"]["source"], "none")
        self.assertEqual(inspect_font_cache(user_data_root=user_data)[0], "OK")

    def test_remove_keeps_the_entry_of_a_slot_file_it_cannot_delete(self) -> None:
        source = self._folder("all", {"a.pgf": japanese_pgf(), "b.pgf": latin_pgf(), "c.pgf": korean_pgf()})
        user_data = self.temp_path / "ud"
        import_fonts(source, user_data_root=user_data)
        cache = get_font_cache_dir(user_data)
        manifest_path = cache / "manifest.json"
        imported = json.loads(manifest_path.read_text(encoding="utf-8"))["files"]
        # A non-empty directory at the Latin slot's file name cannot be deleted as a file.
        (cache / "nkltn.pgf").unlink()
        (cache / "nkltn.pgf").mkdir()
        (cache / "nkltn.pgf" / "keep.txt").write_bytes(b"not a font")

        # The failing slot is named first: the healthy slot after it is still removed.
        with self.assertRaisesRegex(FontImportError, r"Latin font file nkltn\.pgf could not be removed") as raised:
            remove_imports(user_data_root=user_data, slots=["latin", "japanese"])
        self.assertIn("Removed 1 other imported font file(s).", str(raised.exception))
        self.assertFalse((cache / "nkjpn.pgf").exists())
        self.assertEqual((cache / "nkltn.pgf" / "keep.txt").read_bytes(), b"not a font")
        self.assertTrue((cache / "nkkr.pgf").is_file())
        files = json.loads(manifest_path.read_text(encoding="utf-8"))["files"]
        self.assertEqual(sorted(files), ["nkkr.pgf", "nkltn.pgf"])
        self.assertEqual(files["nkltn.pgf"], imported["nkltn.pgf"])
        self.assertEqual(files["nkkr.pgf"], imported["nkkr.pgf"])

        # Naming only the failing slot drops no entry, so the manifest is not rewritten: a
        # rewrite would replace this import_time. The command reports the slot and exits 1.
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["import_time"] = "2000-01-01T00:00:00Z"
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        before = manifest_path.read_bytes()
        res = self._cli("remove", "--slot", "latin", "--user-data-root", str(user_data))
        self.assertEqual(res.returncode, 1, res.stdout)
        self.assertIn("Font remove error: The Latin font file nkltn.pgf could not be removed", res.stderr)
        self.assertEqual(res.stdout, "")
        self.assertEqual(manifest_path.read_bytes(), before)

    def test_malformed_or_old_manifest_is_invalid(self) -> None:
        user_data = self.temp_path / "ud"
        cache = get_font_cache_dir(user_data)
        cache.mkdir(parents=True)
        (cache / "manifest.json").write_text("{invalid json", encoding="utf-8")
        status, message = inspect_font_cache(user_data_root=user_data)
        self.assertEqual(status, "INVALID")
        self.assertIn("run fonts import <folder>", message)
        (cache / "manifest.json").write_text(json.dumps({"schema_version": 1, "files": {}}), encoding="utf-8")
        self.assertEqual(inspect_font_cache(user_data_root=user_data)[0], "INVALID")
        (cache / "manifest.json").write_text(json.dumps({
            "schema_version": 2,
            "files": {"nkjpn.pgf": {"slot": "japanese", "size": 10, "sha256": "0" * 64, "source": "user"}},
        }), encoding="utf-8")
        self.assertEqual(inspect_font_cache(user_data_root=user_data)[0], "INVALID")

    def test_empty_folder_is_refused(self) -> None:
        empty = self._folder("empty", {})
        with self.assertRaisesRegex(FontImportError, "No .pgf files"):
            import_fonts(empty, user_data_root=self.temp_path / "ud")

    # The command line ------------------------------------------------------------------------

    def _cli(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "fonts", *args],
            capture_output=True,
            text=True,
            cwd=ROOT,
            check=False,
        )

    def test_cli_import_status_and_remove(self) -> None:
        source = self._folder("cli_dump", {"one.pgf": latin_pgf(), "two.pgf": japanese_pgf()})
        user_data = self.temp_path / "cli_user_data"
        res = self._cli("import", str(source), "--user-data-root", str(user_data), "--json")
        self.assertEqual(res.returncode, 0, res.stderr)
        data = json.loads(res.stdout)
        self.assertEqual(data["status"], "OK")
        self.assertEqual(data["imported_count"], 2)
        self.assertIn("nkltn.pgf", data["files"])

        status = self._cli("status", "--user-data-root", str(user_data), "--project-root", str(self.temp_path), "--json")
        self.assertEqual(status.returncode, 0, status.stderr)
        states = json.loads(status.stdout)
        self.assertEqual(states["japanese"]["source"], "user")
        self.assertEqual(states["korean"]["source"], "none")

        removed = self._cli("remove", "--all", "--user-data-root", str(user_data))
        self.assertEqual(removed.returncode, 0, removed.stderr)
        self.assertIn("Removed 2 imported font file(s).", removed.stdout)

    def test_cli_import_choose_and_refusal(self) -> None:
        dupes = self._folder("cli_dupes", {"x.pgf": latin_pgf(), "y.pgf": latin_pgf()})
        user_data = self.temp_path / "cli_user_data"
        bad_choice = self._cli("import", str(dupes), "--user-data-root", str(user_data),
                               "--choose", "latin=nope.pgf")
        self.assertEqual(bad_choice.returncode, 1)
        self.assertIn("Font import error", bad_choice.stderr)
        chosen = self._cli("import", str(dupes), "--user-data-root", str(user_data),
                           "--choose", "latin=y.pgf")
        self.assertEqual(chosen.returncode, 0, chosen.stderr)
        bad = self._folder("cli_bad", {"junk.pgf": b"not a valid font"})
        res = self._cli("import", str(bad), "--user-data-root", str(self.temp_path / "other"))
        self.assertEqual(res.returncode, 1)
        self.assertIn("Font import error", res.stderr)

    # Policy and documentation ------------------------------------------------------------------

    def test_public_candidate_policy_cannot_include_user_data_fonts(self) -> None:
        """Public source candidate and policy never include user-data fonts."""
        from tools import publication_policy

        policy = publication_policy.load_policy(ROOT / "assets" / "public_source_profile.json")
        for cand in (
            "fonts/v2/manifest.json",
            "fonts/v2/nkjpn.pgf",
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
        """README and docs/SETUP.md document the import command, the v2 cache, and the slot status."""
        readme_text = (ROOT / "README.md").read_text(encoding="utf-8")
        setup_text = (ROOT / "docs" / "SETUP.md").read_text(encoding="utf-8")

        self.assertIn("tools/nk_cli.py fonts import", readme_text)
        self.assertIn("#313", readme_text)

        self.assertIn("### System fonts", setup_text)
        self.assertIn("python tools/nk_cli.py fonts import <folder>", setup_text)
        self.assertIn("python tools/nk_cli.py fonts status", setup_text)
        self.assertIn("python tools/nk_cli.py fonts remove", setup_text)
        self.assertIn("fonts/v2", setup_text)
        self.assertNotIn("fonts/v1", setup_text)
        self.assertIn("manifest.json", setup_text)
        self.assertIn("SYSTEM_FONTS", setup_text)
        self.assertIn("#300", setup_text)
        self.assertIn("#313", setup_text)


if __name__ == "__main__":
    unittest.main()
