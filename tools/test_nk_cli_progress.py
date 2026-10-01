# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Unit tests for nk_cli.py build-package --progress-json and --log-file options."""

from __future__ import annotations

import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))

ROOT = Path(__file__).resolve().parent.parent
CLI_PATH = ROOT / "tools" / "nk_cli.py"


class NkCliProgressTests(unittest.TestCase):
    def test_launch_anchors_runtime_lookup_to_cli_checkout(self) -> None:
        import argparse
        from contextlib import redirect_stderr, redirect_stdout
        from io import StringIO
        from unittest.mock import patch

        import nk_cli

        with tempfile.TemporaryDirectory(prefix="nk_cli_launch_paths_") as temp:
            temp_root = Path(temp)
            cli_root = temp_root / "cli checkout"
            other_cwd = temp_root / "invoking checkout"
            game_dir = temp_root / "prepared game"
            runtime_dir = cli_root / "build" / "synthetic"
            runtime_dir.mkdir(parents=True)
            other_cwd.mkdir()
            game_dir.mkdir()

            executable = runtime_dir / "synthetic.exe"
            executable.write_bytes(b"synthetic runtime fixture")
            (runtime_dir / "synthetic_image.bin").write_bytes(b"synthetic image fixture")
            (game_dir / "manifest.json").write_text(
                json.dumps({"title_id": "synthetic-allegrex-v1"}),
                encoding="utf-8",
            )

            args = argparse.Namespace(
                game_dir=game_dir,
                profile="Standard",
                fps_cap=30,
                software=False,
            )
            stdout = StringIO()
            stderr = StringIO()
            original_cwd = Path.cwd()
            try:
                os.chdir(other_cwd)
                with (
                    patch.object(nk_cli, "ROOT", cli_root),
                    patch.object(nk_cli.subprocess, "Popen") as popen,
                    redirect_stdout(stdout),
                    redirect_stderr(stderr),
                ):
                    result = nk_cli.cmd_launch(args)
            finally:
                os.chdir(original_cwd)

        popen.assert_not_called()
        self.assertEqual(result, 0, stderr.getvalue())
        self.assertIn(f"Executable: {executable}", stdout.getvalue())

    def test_progress_json_invalid_disc_id(self) -> None:
        """Verify --progress-json reports failure for an invalid disc ID format."""
        cmd = [
            sys.executable,
            str(CLI_PATH),
            "build-package",
            "INVALID_DISC",
            "--progress-json",
        ]
        proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        self.assertNotEqual(proc.returncode, 0)
        lines = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
        self.assertGreaterEqual(len(lines), 1)
        events = [json.loads(line) for line in lines]
        self.assertTrue(all("stage" in ev and "status" in ev and "message" in ev for ev in events))
        self.assertEqual(events[-1]["status"], "FAIL")
        self.assertEqual(events[-1]["stage"], "preflight")

    def test_progress_json_nonexistent_disc_id(self) -> None:
        """Verify --progress-json reports failure when the library entry does not exist."""
        with tempfile.TemporaryDirectory(prefix="nk_cli_prog_test_") as tmpdir:
            user_root = Path(tmpdir)
            cmd = [
                sys.executable,
                str(CLI_PATH),
                "build-package",
                "TEST99999",
                "--user-data-root",
                str(user_root),
                "--progress-json",
            ]
            proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
            self.assertNotEqual(proc.returncode, 0)
            lines = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
            self.assertGreaterEqual(len(lines), 1)
            events = [json.loads(line) for line in lines]
            self.assertEqual(events[0]["status"], "START")
            self.assertEqual(events[0]["stage"], "preflight")
            self.assertEqual(events[-1]["status"], "FAIL")
            self.assertIn("stage", events[-1])

    def test_progress_json_to_file(self) -> None:
        """Verify --progress-json <file> writes JSON lines to a file."""
        with tempfile.TemporaryDirectory(prefix="nk_cli_prog_test_") as tmpdir:
            user_root = Path(tmpdir)
            progress_file = user_root / "progress.jsonl"
            log_file = user_root / "build.log"
            cmd = [
                sys.executable,
                str(CLI_PATH),
                "build-package",
                "TEST99999",
                "--user-data-root",
                str(user_root),
                "--progress-json",
                str(progress_file),
                "--log-file",
                str(log_file),
            ]
            proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
            self.assertNotEqual(proc.returncode, 0)
            self.assertTrue(progress_file.is_file(), "Progress file was not created")
            lines = [line.strip() for line in progress_file.read_text(encoding="utf-8").splitlines() if line.strip()]
            self.assertGreaterEqual(len(lines), 1)
            events = [json.loads(line) for line in lines]
            self.assertEqual(events[-1]["status"], "FAIL")
            self.assertTrue(log_file.is_file(), "Log file was not created")
            log_content = log_file.read_text(encoding="utf-8")
            self.assertIn("preflight", log_content)

    def test_human_output_unchanged_when_no_progress_flag(self) -> None:
        """Verify human output is printed when --progress-json is omitted."""
        with tempfile.TemporaryDirectory(prefix="nk_cli_prog_test_") as tmpdir:
            user_root = Path(tmpdir)
            cmd = [
                sys.executable,
                str(CLI_PATH),
                "build-package",
                "TEST99999",
                "--user-data-root",
                str(user_root),
            ]
            proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
            self.assertNotEqual(proc.returncode, 0)
            # Output should not contain JSON objects
            for line in proc.stdout.splitlines():
                with self.assertRaises(json.JSONDecodeError):
                    json.loads(line)

    def test_progress_json_reports_boundary_on_encrypted_iso(self) -> None:
        """Verify --progress-json reports the fail-closed boundary and issue #295 for encrypted ISO."""
        from test_iso_parity import build_psp_container, create_test_iso_with_executables

        with tempfile.TemporaryDirectory(prefix="nk_cli_prog_boundary_") as tmpdir:
            user_root = Path(tmpdir)
            iso_path = user_root / "encrypted.iso"
            create_test_iso_with_executables(
                iso_path, build_psp_container(), disc_id="ULUS99998"
            )
            library = {
                "schema_version": 1,
                "games": [{
                    "disc_id": "ULUS99998",
                    "title_id": "experimental-ulus99998",
                    "iso_path": str(iso_path),
                    "selected_executable": "EBOOT.BIN",
                    "is_experimental": True,
                }],
            }
            (user_root / "library.json").write_text(json.dumps(library), encoding="utf-8")
            cmd = [
                sys.executable,
                str(CLI_PATH),
                "build-package",
                "ULUS99998",
                "--user-data-root",
                str(user_root),
                "--progress-json",
            ]
            proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
            self.assertNotEqual(proc.returncode, 0)
            lines = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
            self.assertGreaterEqual(len(lines), 1)
            events = [json.loads(line) for line in lines]
            fail_event = events[-1]
            self.assertEqual(fail_event["status"], "FAIL")
            self.assertIn("in the works", fail_event["message"])
            self.assertIn("#295", fail_event["message"])


class NkCliVramTests(unittest.TestCase):
    @staticmethod
    def _read_png(path: Path) -> tuple[int, int, list[tuple[int, int, int, int]]]:
        payload = path.read_bytes()
        if payload[:8] != b"\x89PNG\r\n\x1a\n":
            raise AssertionError("output is not a PNG")
        offset = 8
        compressed = bytearray()
        width = height = 0
        while offset < len(payload):
            size = struct.unpack_from(">I", payload, offset)[0]
            kind = payload[offset + 4:offset + 8]
            content = payload[offset + 8:offset + 8 + size]
            offset += 12 + size
            if kind == b"IHDR":
                width, height, depth, color, *_ = struct.unpack(">IIBBBBB", content)
                if depth != 8 or color != 6:
                    raise AssertionError("output PNG is not RGBA8")
            elif kind == b"IDAT":
                compressed.extend(content)
            elif kind == b"IEND":
                break
        raw = zlib.decompress(compressed)
        row_bytes = width * 4
        pixels = []
        for y in range(height):
            row = raw[y * (row_bytes + 1):(y + 1) * (row_bytes + 1)]
            if row[0] != 0:
                raise AssertionError("test PNG writer used an unsupported PNG filter")
            pixels.extend(tuple(row[i:i + 4]) for i in range(1, len(row), 4))
        return width, height, pixels

    def _make_fixture(self, root: Path) -> tuple[Path, dict[str, list[tuple[int, int, int, int]]]]:
        image = bytearray(2 * 1024 * 1024)
        surfaces = []
        expected = {}

        def add(name: str, offset: int, width: int, height: int, stride: int,
                fmt: str, pixels, *, swizzled=False, kind="texture"):
            surfaces.append({
                "name": name, "kind": kind, "addr": hex(0x04000000 + offset),
                "width": width, "height": height, "stride": stride,
                "format": fmt, "format_code": {
                    "5650": 0, "5551": 1, "4444": 2, "8888": 3,
                    "CLUT4": 4, "CLUT8": 5, "DXT1": 8, "DXT3": 9,
                    "DXT5": 10, "DEPTH16": 11,
                }[fmt], "swizzled": swizzled,
            })
            expected[name] = pixels

        struct.pack_into("<HHH", image, 0, 0x001F, 0xFC00, 0xF0F0)
        image[6:10] = bytes((17, 34, 51, 255))
        add("color5650", 0, 1, 1, 1, "5650", [(255, 0, 0, 255)], kind="color")
        add("color5551", 2, 1, 1, 1, "5551", [(0, 0, 255, 255)], kind="color")
        add("color4444", 4, 1, 1, 1, "4444", [(0, 255, 0, 255)], kind="color")
        add("color8888", 6, 1, 1, 1, "8888", [(17, 34, 51, 255)], kind="color")

        palette = struct.pack("<HHHH", 0x001F, 0x07E0, 0xF800, 0xFFFF)
        image[12:14] = bytes((0x10, 0x00))
        image[14:16] = bytes((1, 0))
        add("indexed4", 12, 2, 1, 2, "CLUT4", [(255, 0, 0, 255), (0, 255, 0, 255)])
        add("indexed8", 14, 2, 1, 2, "CLUT8", [(0, 255, 0, 255), (255, 0, 0, 255)])

        image[32:40] = bytes((0, 0, 0, 0, 0x1F, 0, 0xE0, 0x07))
        add("dxt1", 32, 4, 4, 4, "DXT1", [(255, 0, 0, 255)] * 16)
        image[40:48] = bytes((0, 0, 0, 0, 0x1F, 0, 0xE0, 0x07))
        image[48:56] = bytes((0xFF,) * 8)
        add("dxt3", 40, 4, 4, 4, "DXT3", [(255, 0, 0, 255)] * 16)
        image[56:64] = bytes((0, 0, 0, 0, 0x1F, 0, 0xE0, 0x07))
        image[70:72] = bytes((255, 0))
        add("dxt5", 56, 4, 4, 4, "DXT5", [(255, 0, 0, 255)] * 16)
        struct.pack_into("<HH", image, 80, 0x001F, 0x07E0)
        struct.pack_into("<HH", image, 96, 0xF800, 0xFFFF)
        add("swizzled", 80, 2, 2, 8, "5650", [
            (255, 0, 0, 255), (0, 255, 0, 255),
            (0, 0, 255, 255), (255, 255, 255, 255),
        ], swizzled=True)
        struct.pack_into("<H", image, 112, 0x7F00)
        add("depth", 112, 1, 1, 1, "DEPTH16", [(127, 127, 127, 255)], kind="depth")
        palette_hex = palette.hex()
        surfaces.append({
            "name": "clut_preview", "kind": "palette", "addr": "0x04000000",
            "width": 4, "height": 1, "stride": 4, "format": "5650",
            "format_code": 0, "swizzled": False, "data_hex": palette_hex,
        })
        expected["clut_preview"] = [
            (255, 0, 0, 255), (0, 255, 0, 255),
            (0, 0, 255, 255), (255, 255, 255, 255),
        ]
        image_path = root / "vram_12.bin"
        image_path.write_bytes(image)
        sidecar_path = root / "vram_12.json"
        sidecar_path.write_text(json.dumps({
            "schema": "nakagawa-vram-dump-v1", "vblank": 12,
            "vram": {"base": "0x04000000", "size_bytes": len(image),
                     "image_file": image_path.name},
            "texture_levels": [],
            "clut": {"addr": "0x04000000", "size_bytes": len(palette),
                     "stride": 4, "format": "5650", "format_code": 0,
                     "format_word": "0x0000ff00", "swizzled": False,
                     "loaded_data_hex": (palette + bytes(2048 - len(palette))).hex()},
            "surfaces": surfaces,
        }), encoding="utf-8")
        expected["dxt1"] = [(255, 0, 0, 255)] * 16
        expected["dxt3"] = [(255, 0, 0, 255)] * 16
        expected["dxt5"] = [(255, 0, 0, 255)] * 16
        return sidecar_path, expected

    def test_vram_sidecar_decodes_supported_formats_to_expected_png_pixels(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk_cli_vram_fixture_") as temp:
            root = Path(temp)
            sidecar, expected = self._make_fixture(root)
            output = root / "png"
            process = subprocess.run(
                [sys.executable, str(CLI_PATH), "vram", "--from-sidecar", str(sidecar),
                 "--out-dir", str(output)],
                cwd=ROOT, capture_output=True, text=True,
            )
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            for name, expected_pixels in expected.items():
                with self.subTest(surface=name):
                    width, height, pixels = self._read_png(output / f"{name}.png")
                    self.assertEqual(len(pixels), width * height)
                    self.assertEqual(pixels, expected_pixels)

    def test_vram_direct_export_decodes_one_linear_surface(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk_cli_vram_direct_") as temp:
            root = Path(temp)
            sidecar, _expected = self._make_fixture(root)
            output = root / "single.png"
            process = subprocess.run(
                [sys.executable, str(CLI_PATH), "vram", str(sidecar.with_name("vram_12.bin")),
                 "--addr", "0x04000000", "--width", "1", "--height", "1",
                 "--stride", "1", "--format", "5650", "--output", str(output)],
                cwd=ROOT, capture_output=True, text=True,
            )
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            self.assertEqual(self._read_png(output)[2], [(255, 0, 0, 255)])


if __name__ == "__main__":
    unittest.main()
