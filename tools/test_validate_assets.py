# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Synthetic, retail-free hostile coverage for tools/validate_assets.py.

Every fixture here is generated in-process or in a temporary directory.  No
archive, texture, name, or hash from a retail title appears in this file.

The contract under test (issue #376) is fail-closed asset verification:

- a discovered inventory_map.json that cannot be read, parsed, or validated
  against the extractor's schema must fail the run, never disappear into it;
- zero successfully parsed inventories cannot report SUCCESS;
- a .gim-classified asset with an invalid or truncated GIM header is a
  corruption failure, not a successful palette skip;
- GIM block fields are bounds-checked before every read, with the offending
  block in the diagnostic.

Each scenario asserts the process exit status as well as the printed
diagnostics, so a visible error can never coexist with exit 0.
"""

from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parent / "validate_assets.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("validate_assets_under_test", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


va = _load_module()


# ---------------------------------------------------------------------------
# Fixture builders (all bytes synthetic)
# ---------------------------------------------------------------------------


def _block(block_id: int, content: bytes, *, header_size: int = 16, size: int | None = None) -> bytes:
    actual_size = 16 + len(content) if size is None else size
    header = bytearray(16)
    struct.pack_into("<H", header, 0, block_id)
    struct.pack_into("<I", header, 4, actual_size)
    struct.pack_into("<I", header, 12, header_size)
    return bytes(header) + content


def _image_block(*, fmt: int = 5, raw: bytes = b"\x00\x01\x02\x03") -> bytes:
    content = bytearray(36)
    struct.pack_into("<H", content, 4, fmt)
    struct.pack_into("<I", content, 28, 36)
    struct.pack_into("<I", content, 32, 36 + len(raw))
    return _block(0x0004, bytes(content) + raw)


def _palette_block(*, raw: bytes = b"\x00\x00\xff\xff", start: int = 36, end: int | None = None) -> bytes:
    content = bytearray(36)
    struct.pack_into("<I", content, 28, start)
    struct.pack_into("<I", content, 32, start + len(raw) if end is None else end)
    return _block(0x0005, bytes(content) + raw)


def _gim(*blocks: bytes) -> bytes:
    return b"MIG.00.1PSP" + b"\0" * 5 + b"".join(blocks)


VALID_T8_GIM = _gim(_image_block(fmt=5), _palette_block())


def _producer_inventory(**overrides) -> dict:
    """The exact shape tools/extract_xb.py process_extracted_directory writes."""
    inv = {
        "textures": [],
        "sounds": [],
        "scene_graphs": [],
        "other": [{"name": "data.bin", "path": "data.bin", "size_bytes": 8}],
        "rejected": [],
    }
    inv.update(overrides)
    return inv


def _write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def _write_json(path: Path, payload) -> None:
    _write_bytes(path, json.dumps(payload, indent=4).encode("utf-8"))


def _write_inventory(root: Path, subdir: str, *, inv=None, raw=None) -> Path:
    """Materialize one archive directory containing an inventory_map.json."""
    target = root / subdir
    target.mkdir(parents=True, exist_ok=True)
    inv_path = target / "inventory_map.json"
    if raw is not None:
        inv_path.write_bytes(raw)
    else:
        _write_json(inv_path, inv if inv is not None else _producer_inventory())
    return inv_path


def _clean_tree(root: Path) -> Path:
    """One valid archive: a T8 texture, an SGD with a resolvable stream link,
    and a stream file.  Returns the archive directory."""
    archive = root / "archive_a"
    _write_bytes(archive / "tex.gim", VALID_T8_GIM)
    _write_bytes(archive / "music" / "jingle.at3", b"synthetic stream bytes")
    _write_bytes(
        archive / "bgm.sgd",
        b"SYNTHETIC\0stream-table\0music/jingle.at3\0",
    )
    _write_json(archive / "inventory_map.json", _producer_inventory(
        textures=[{"name": "tex.gim", "path": "tex.gim", "size_bytes": len(VALID_T8_GIM)}],
        sounds=[
            {"name": "bgm.sgd", "path": "bgm.sgd", "size_bytes": 40},
            {"name": "jingle.at3", "path": "music/jingle.at3", "size_bytes": 21},
        ],
        other=[],
    ))
    return archive


def _write_reference(path: Path, mapping: dict | None = None) -> None:
    _write_json(path, mapping if mapping is not None else {})


def _run_validator(tree: Path, ref: Path, *extra: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-I", str(SCRIPT), "--dir", str(tree), "--reference", str(ref), *extra],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def _combined(process: subprocess.CompletedProcess) -> str:
    return process.stdout + process.stderr


def _count(output: str, label: str) -> int:
    match = re.search(rf"{re.escape(label)}:\s+(\d+)", output)
    return int(match.group(1)) if match else -1


def _make_unreadable(path: Path):
    """Make ``path`` unreadable for the lifetime of the returned undo callable.

    Returns None when the host cannot produce an unreadable file without
    privileges (for example when running as root on POSIX).
    """
    if sys.platform == "win32":
        import ctypes

        kernel32 = ctypes.windll.kernel32
        kernel32.CreateFileW.restype = ctypes.c_void_p
        kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel32.CreateFileW(str(path), 0x80000000, 0, None, 3, 0x80, None)
        invalid = ctypes.c_void_p(-1).value
        if handle in (None, invalid):
            return None
        try:
            with open(path, "rb"):
                pass
        except OSError:
            return lambda: kernel32.CloseHandle(handle)
        kernel32.CloseHandle(handle)
        return None

    os.chmod(path, 0)
    try:
        with open(path, "rb"):
            pass
    except OSError:
        return lambda: os.chmod(path, 0o644)
    os.chmod(path, 0o644)
    return None


# ---------------------------------------------------------------------------
# CLI-level hostile scenarios (exit status + diagnostics)
# ---------------------------------------------------------------------------


class ValidatorCliBase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.base = Path(self._tmp.name)
        self.tree = self.base / "extracted"
        self.tree.mkdir()
        self.ref = self.base / "reference_hashes.json"
        self.addCleanup(self._tmp.cleanup)


class MalformedInventoryTests(ValidatorCliBase):
    def test_malformed_json_inventory_fails_with_status_and_message(self) -> None:
        _write_inventory(self.tree, "archive_bad", raw=b"{not json at all")
        _write_reference(self.ref, {})
        proc = _run_validator(self.tree, self.ref)

        self.assertEqual(proc.returncode, 1, _combined(proc))
        out = _combined(proc)
        self.assertIn("Inventory Index Failures", out)
        self.assertIn("archive_bad", out)
        self.assertIn("malformed JSON", out)
        self.assertIn("Verification FAILED", out)
        self.assertNotIn("Verification SUCCESS", out)
        self.assertNotIn("Traceback", out)

    def test_non_object_inventory_top_level_fails(self) -> None:
        _write_inventory(self.tree, "archive_bad", raw=b'["textures", "sounds"]')
        _write_reference(self.ref, {})
        proc = _run_validator(self.tree, self.ref)

        self.assertEqual(proc.returncode, 1, _combined(proc))
        out = _combined(proc)
        self.assertIn("top-level JSON value must be an object", out)
        self.assertIn("Verification FAILED", out)

    def test_unreadable_inventory_fails_closed(self) -> None:
        inv_path = _write_inventory(self.tree, "archive_locked")
        undo = _make_unreadable(inv_path)
        if undo is None:
            self.skipTest("host cannot produce an unreadable file without privileges")
        try:
            _write_reference(self.ref, {})
            proc = _run_validator(self.tree, self.ref)
        finally:
            undo()

        self.assertEqual(proc.returncode, 1, _combined(proc))
        out = _combined(proc)
        self.assertIn("Inventory Index Failures", out)
        self.assertIn("archive_locked", out)
        self.assertIn("unreadable", out)
        self.assertIn("Verification FAILED", out)
        self.assertNotIn("Verification SUCCESS", out)


class SchemaInventoryTests(ValidatorCliBase):
    def test_wrong_category_shape_fails(self) -> None:
        _write_inventory(self.tree, "archive_bad", inv=_producer_inventory(textures={"a": 1}))
        _write_reference(self.ref, {})
        proc = _run_validator(self.tree, self.ref)

        self.assertEqual(proc.returncode, 1, _combined(proc))
        out = _combined(proc)
        self.assertIn("schema violations", out)
        self.assertIn("category 'textures' must be a list", out)
        self.assertIn("Verification FAILED", out)

    def test_item_with_wrong_value_shapes_fails(self) -> None:
        bad = _producer_inventory(other=[{
            "name": "data.bin",
            "path": "data.bin",
            "size_bytes": "8",
        }])
        _write_inventory(self.tree, "archive_bad", inv=bad)
        _write_reference(self.ref, {})
        proc = _run_validator(self.tree, self.ref)

        self.assertEqual(proc.returncode, 1, _combined(proc))
        self.assertIn("'size_bytes' must be a non-negative integer", _combined(proc))


class MixedInventoryTests(ValidatorCliBase):
    def test_one_bad_inventory_among_good_ones_fails_the_whole_run(self) -> None:
        _clean_tree(self.tree)
        _write_inventory(self.tree, "archive_bad", raw=b"{ truncated")
        _write_reference(self.ref, {})
        proc = _run_validator(self.tree, self.ref)

        self.assertEqual(proc.returncode, 1, _combined(proc))
        out = _combined(proc)
        self.assertIn("archive_bad", out)
        self.assertIn("Verification FAILED", out)
        self.assertNotIn("Verification SUCCESS", out)
        # The good inventory was still fully examined; coverage stays explicit.
        self.assertEqual(_count(out, "Inventories discovered"), 2)
        self.assertEqual(_count(out, "Inventories parsed"), 1)
        self.assertEqual(_count(out, "Inventories failed"), 1)
        self.assertNotIn("No inventory could be parsed", out)

    def test_all_inventories_bad_reports_zero_parsed_and_fails(self) -> None:
        _write_inventory(self.tree, "archive_bad1", raw=b"{ truncated")
        _write_inventory(self.tree, "archive_bad2", inv=_producer_inventory(textures=5))
        _write_reference(self.ref, {})
        proc = _run_validator(self.tree, self.ref)

        self.assertEqual(proc.returncode, 1, _combined(proc))
        out = _combined(proc)
        self.assertEqual(_count(out, "Inventories discovered"), 2)
        self.assertEqual(_count(out, "Inventories parsed"), 0)
        self.assertEqual(_count(out, "Inventories failed"), 2)
        self.assertIn("No inventory could be parsed", out)
        self.assertIn("Verification FAILED", out)
        self.assertNotIn("Verification SUCCESS", out)


class GimAssetTests(ValidatorCliBase):
    def test_truncated_gim_asset_is_a_failure_not_a_successful_skip(self) -> None:
        archive = _clean_tree(self.tree)
        truncated = b"MIG.00.1PSP"  # valid magic prefix, header cut short
        (archive / "broken.gim").write_bytes(truncated)
        inv = json.loads((archive / "inventory_map.json").read_text(encoding="utf-8"))
        inv["textures"].append({
            "name": "broken.gim", "path": "broken.gim", "size_bytes": len(truncated),
        })
        _write_json(archive / "inventory_map.json", inv)
        _write_reference(self.ref, {})
        proc = _run_validator(self.tree, self.ref)

        self.assertEqual(proc.returncode, 1, _combined(proc))
        out = _combined(proc)
        self.assertIn("Palette Integrity Failures", out)
        self.assertIn("broken.gim", out)
        self.assertIn("Truncated GIM header", out)
        self.assertNotIn("Skipped (not a valid GIM header)", out)
        self.assertNotIn("Verification SUCCESS", out)

    def test_gim_asset_with_invalid_magic_is_a_failure(self) -> None:
        archive = _clean_tree(self.tree)
        bogus = b"NOTGIMDATA" + b"\0" * 40
        (archive / "fake.gim").write_bytes(bogus)
        inv = json.loads((archive / "inventory_map.json").read_text(encoding="utf-8"))
        inv["textures"].append({
            "name": "fake.gim", "path": "fake.gim", "size_bytes": len(bogus),
        })
        _write_json(archive / "inventory_map.json", inv)
        _write_reference(self.ref, {})
        proc = _run_validator(self.tree, self.ref)

        self.assertEqual(proc.returncode, 1, _combined(proc))
        out = _combined(proc)
        self.assertIn("Invalid GIM header magic", out)
        self.assertNotIn("Verification SUCCESS", out)


class SgdStreamTests(ValidatorCliBase):
    def test_sgd_referencing_a_missing_stream_fails(self) -> None:
        archive = _clean_tree(self.tree)
        (archive / "bgm.sgd").write_bytes(b"SYNTHETIC\0music/ghost.at3\0")
        _write_reference(self.ref, {})
        proc = _run_validator(self.tree, self.ref)

        self.assertEqual(proc.returncode, 1, _combined(proc))
        out = _combined(proc)
        self.assertIn("Missing Sound Streams / Broken links", out)
        self.assertIn("music/ghost.at3", out)
        self.assertNotIn("Verification SUCCESS", out)

    def test_a_clean_tree_reports_zero_sound_link_failures(self) -> None:
        _clean_tree(self.tree)
        _write_reference(self.ref, {})
        proc = _run_validator(self.tree, self.ref)

        self.assertEqual(proc.returncode, 0, _combined(proc))
        self.assertEqual(_count(_combined(proc), "Sound link failures"), 0)

    def test_stream_link_resolution_classes(self) -> None:
        # Passing link checks print nothing at the CLI level, so the match
        # classes are asserted directly against the checker.
        with tempfile.TemporaryDirectory() as tmp:
            sgd = Path(tmp) / "bgm.sgd"
            sgd.write_bytes(b"SYNTHETIC\0music/jingle.at3\0")

            ok, msg = va.check_sgd_stream_links(
                str(sgd), {"jingle.at3"}, {"music/jingle.at3"}
            )
            self.assertTrue(ok, msg)
            self.assertEqual(msg, "OK (verified 1 stream references)")

            ok, msg = va.check_sgd_stream_links(str(sgd), {"jingle.at3"}, set())
            self.assertTrue(ok, msg)
            self.assertIn("ambiguous basename matches: music/jingle.at3", msg)

            ok, msg = va.check_sgd_stream_links(str(sgd), set(), set())
            self.assertFalse(ok)
            self.assertEqual(msg, "Missing target stream files: music/jingle.at3")


class CleanControlTests(ValidatorCliBase):
    def test_clean_tree_bootstraps_and_revalidates_with_exit_zero(self) -> None:
        _clean_tree(self.tree)
        ref = self.base / "bootstrapped_hashes.json"

        proc = _run_validator(self.tree, ref, "--bootstrap")
        self.assertEqual(proc.returncode, 0, _combined(proc))
        self.assertIn("Verification SUCCESS", _combined(proc))
        self.assertTrue(ref.is_file())
        bootstrapped = json.loads(ref.read_text(encoding="utf-8"))
        self.assertEqual(len(bootstrapped), 3)  # tex.gim, bgm.sgd, music/jingle.at3
        self.assertIn("archive_a/tex.gim", bootstrapped)

        # A re-run against the freshly bootstrapped manifest must stay clean.
        proc = _run_validator(self.tree, ref)
        self.assertEqual(proc.returncode, 0, _combined(proc))
        self.assertIn("Verification SUCCESS", _combined(proc))
        self.assertEqual(_count(_combined(proc), "Inventories parsed"), 1)
        self.assertEqual(_count(_combined(proc), "Hash mismatches"), 0)

    def test_empty_reference_is_a_warning_not_a_failure_without_strict(self) -> None:
        _clean_tree(self.tree)
        _write_reference(self.ref, {})
        proc = _run_validator(self.tree, self.ref)

        self.assertEqual(proc.returncode, 0, _combined(proc))
        out = _combined(proc)
        self.assertIn("missing from golden reference hashes", out)
        self.assertIn("Verification SUCCESS", out)


class GoldenHashTests(ValidatorCliBase):
    def test_corrupted_asset_against_golden_reference_fails(self) -> None:
        archive = _clean_tree(self.tree)
        ref = self.base / "bootstrapped_hashes.json"
        self.assertEqual(_run_validator(self.tree, ref, "--bootstrap").returncode, 0)

        # Flip one byte of the texture after bootstrapping.
        data = bytearray((archive / "tex.gim").read_bytes())
        data[-1] ^= 0xFF
        (archive / "tex.gim").write_bytes(bytes(data))

        proc = _run_validator(self.tree, ref)
        self.assertEqual(proc.returncode, 1, _combined(proc))
        out = _combined(proc)
        self.assertIn("Hash Corruptions / Mismatches", out)
        self.assertIn("Hash mismatch", out)
        self.assertNotIn("Verification SUCCESS", out)

    def test_asset_declared_but_missing_on_disk_fails(self) -> None:
        archive = _clean_tree(self.tree)
        (archive / "tex.gim").unlink()
        _write_reference(self.ref, {})
        proc = _run_validator(self.tree, self.ref)

        self.assertEqual(proc.returncode, 1, _combined(proc))
        self.assertIn("File missing on disk", _combined(proc))


class BootstrapIntegrityTests(ValidatorCliBase):
    def test_bootstrap_with_a_bad_inventory_fails_and_writes_no_manifest(self) -> None:
        _clean_tree(self.tree)
        _write_inventory(self.tree, "archive_bad", raw=b"{ truncated")
        ref = self.base / "bootstrapped_hashes.json"

        proc = _run_validator(self.tree, ref, "--bootstrap")

        self.assertEqual(proc.returncode, 1, _combined(proc))
        self.assertFalse(ref.exists(), "a manifest must never be written from an incompletely examined tree")
        self.assertIn("Verification FAILED", _combined(proc))


# ---------------------------------------------------------------------------
# Unit-level checks of the schema validator and the GIM parser
# ---------------------------------------------------------------------------


class InventoryShapeTests(unittest.TestCase):
    def test_producer_shape_validates_cleanly(self) -> None:
        self.assertEqual(va.validate_inventory_shape(_producer_inventory()), [])

    def test_pre_rejected_extractor_shape_still_validates(self) -> None:
        inv = _producer_inventory()
        del inv["rejected"]
        self.assertEqual(va.validate_inventory_shape(inv), [])

    def test_missing_and_mistyped_categories_are_reported(self) -> None:
        problems = va.validate_inventory_shape({"textures": []})
        self.assertTrue(any("missing required category 'sounds'" in p for p in problems))

        problems = va.validate_inventory_shape(_producer_inventory(textures={"a": 1}))
        self.assertTrue(any("category 'textures' must be a list" in p for p in problems))

        problems = va.validate_inventory_shape(_producer_inventory(rejected={"a": 1}))
        self.assertTrue(any("category 'rejected' must be a list" in p for p in problems))

    def test_item_shape_matrix(self) -> None:
        cases = {
            "not an object": ["plain string"],
            "missing name": [{"path": "a.bin", "size_bytes": 1}],
            "missing path": [{"name": "a.bin", "size_bytes": 1}],
            "missing size": [{"name": "a.bin", "path": "a.bin"}],
            "empty name": [{"name": "", "path": "a.bin", "size_bytes": 1}],
            "empty path": [{"name": "a.bin", "path": "", "size_bytes": 1}],
            "negative size": [{"name": "a.bin", "path": "a.bin", "size_bytes": -1}],
            "string size": [{"name": "a.bin", "path": "a.bin", "size_bytes": "1"}],
            "boolean size": [{"name": "a.bin", "path": "a.bin", "size_bytes": True}],
            "escaping path": [{"name": "a.bin", "path": "../a.bin", "size_bytes": 1}],
            "absolute path": [{"name": "a.bin", "path": "/etc/a.bin", "size_bytes": 1}],
            "drive path": [{"name": "a.bin", "path": "C:" + chr(92) + "a.bin", "size_bytes": 1}],
        }
        for label, items in cases.items():
            with self.subTest(case=label):
                problems = va.validate_inventory_shape(_producer_inventory(other=items))
                self.assertTrue(problems, f"{label} must be rejected")

    def test_rejected_items_require_a_string_reason(self) -> None:
        problems = va.validate_inventory_shape(_producer_inventory(rejected=[{"name": "x"}]))
        self.assertTrue(any("rejected[0]" in p for p in problems))

    def test_extra_item_keys_are_tolerated(self) -> None:
        items = [{"name": "tex.gim", "path": "tex.gim", "size_bytes": 4, "png_path": "tex.png"}]
        self.assertEqual(va.validate_inventory_shape(_producer_inventory(textures=items)), [])


class GimPaletteUnitTests(unittest.TestCase):
    def test_valid_t8_fixture_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "ok.gim"
            path.write_bytes(VALID_T8_GIM)
            ok, msg = va.check_gim_palette(str(path))
            self.assertTrue(ok, msg)
            self.assertIn("Valid T8 palette", msg)

    def test_short_or_missing_header_is_a_failure_with_diagnostic(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            empty = Path(tmp) / "empty.gim"
            empty.write_bytes(b"")
            ok, msg = va.check_gim_palette(str(empty))
            self.assertFalse(ok)
            self.assertIn("Truncated GIM header", msg)

            short = Path(tmp) / "short.gim"
            short.write_bytes(b"MIG.00.1PSP")
            ok, msg = va.check_gim_palette(str(short))
            self.assertFalse(ok)
            self.assertIn("Truncated GIM header", msg)

    def test_wrong_magic_is_a_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "wrong.gim"
            path.write_bytes(b"XIG.00.1PSP" + b"\0" * 45)
            ok, msg = va.check_gim_palette(str(path))
            self.assertFalse(ok)
            self.assertIn("Invalid GIM header magic", msg)

    def test_unreadable_file_is_a_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ok, msg = va.check_gim_palette(str(Path(tmp) / "does_not_exist.gim"))
            self.assertFalse(ok)
            self.assertIn("Failed to read file", msg)

    def test_hostile_block_headers_fail_with_block_context(self) -> None:
        cases = {
            "zero block size": _gim(_block(0x0004, b"", size=0)),
            "block below header size": _gim(_block(0x0004, b"", size=8)),
            "block past end of data": _gim(_block(0x0004, b"", size=48)),
            "body too short for a block header": b"MIG.00.1PSP" + b"\0" * 5 + b"\x04\0",
            "header size zero": _gim(_block(0x0004, b"abcd", header_size=0)),
            "header size above block": _gim(_block(0x0004, b"abcd", header_size=64)),
            "field read past end": _gim(_block(0x0004, b"")),
        }
        for label, blob in cases.items():
            with self.subTest(case=label):
                with tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp) / "bad.gim"
                    path.write_bytes(blob)
                    ok, msg = va.check_gim_palette(str(path))
                    self.assertFalse(ok, f"{label} must fail")
                    self.assertTrue(
                        "Corrupted GIM block structure" in msg or "Truncated GIM" in msg,
                        msg,
                    )
                    self.assertNotIn("Traceback", msg)

    def test_palette_bounds_are_checked_against_their_block(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            past_end = Path(tmp) / "past.gim"
            past_end.write_bytes(_gim(_image_block(fmt=5), _palette_block(start=36, end=0x1000)))
            ok, msg = va.check_gim_palette(str(past_end))
            self.assertFalse(ok)
            self.assertIn("extends past its block end", msg)

            inverted = Path(tmp) / "inverted.gim"
            inverted.write_bytes(_gim(_image_block(fmt=5), _palette_block(start=40, end=36)))
            ok, msg = va.check_gim_palette(str(inverted))
            self.assertFalse(ok)
            self.assertIn("inverted", msg)

    def test_indexed_formats_still_require_a_palette(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "nopal.gim"
            path.write_bytes(_gim(_image_block(fmt=4)))
            ok, msg = va.check_gim_palette(str(path))
            self.assertFalse(ok)
            self.assertIn("requires a palette", msg)

            empty_pal = Path(tmp) / "emptypal.gim"
            empty_pal.write_bytes(_gim(_image_block(fmt=5), _palette_block(raw=b"", start=36, end=36)))
            ok, msg = va.check_gim_palette(str(empty_pal))
            self.assertFalse(ok)
            self.assertIn("0-byte", msg)

    def test_nested_container_blocks_are_scanned(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "nested.gim"
            path.write_bytes(_gim(_block(0x0002, _image_block(fmt=5) + _palette_block())))
            ok, msg = va.check_gim_palette(str(path))
            self.assertTrue(ok, msg)
            self.assertIn("Valid T8 palette", msg)

    def test_deep_nesting_is_bounded(self) -> None:
        nested = _image_block(fmt=0)
        for _ in range(20):
            nested = _block(0x0002, nested)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "deep.gim"
            path.write_bytes(_gim(nested))
            ok, msg = va.check_gim_palette(str(path))
            self.assertFalse(ok)
            self.assertIn("nesting deeper", msg)


class ExitCodeContractTests(ValidatorCliBase):
    def test_missing_target_directory_exits_nonzero(self) -> None:
        proc = _run_validator(self.base / "does_not_exist", self.ref)
        self.assertEqual(proc.returncode, 1)
        self.assertIn("Target directory does not exist", _combined(proc))

    def test_tree_without_inventories_exits_nonzero(self) -> None:
        _write_bytes(self.tree / "loose.bin", b"synthetic")
        _write_reference(self.ref, {})
        proc = _run_validator(self.tree, self.ref)
        self.assertEqual(proc.returncode, 1)
        self.assertIn("No inventory_map.json indices found", _combined(proc))

    def test_missing_reference_without_bootstrap_exits_two(self) -> None:
        _clean_tree(self.tree)
        proc = _run_validator(self.tree, self.base / "absent.json")
        self.assertEqual(proc.returncode, 2)
        self.assertIn("not found", _combined(proc))


if __name__ == "__main__":
    unittest.main()
