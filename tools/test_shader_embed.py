# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Tests for deterministic checked-in Vulkan shader provenance."""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parent))

import shader_embed


class ShaderEmbedTests(unittest.TestCase):
    def test_checked_in_sources_and_embeddings_match_manifest(self) -> None:
        self.assertEqual(shader_embed.verify(shader_embed.GPU_DIR, shader_embed.MANIFEST), [])
        manifest = json.loads(shader_embed.MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual(manifest["compiler"]["target_environment"], "vulkan1.1")
        self.assertTrue(manifest["compiler"]["version"])

    def test_source_or_embedding_change_is_rejected_without_glslc(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nakagawa-shader-test-") as temp_name:
            gpu_dir = Path(temp_name)
            (gpu_dir / "shaders").mkdir()
            shutil.copy2(shader_embed.MANIFEST, gpu_dir / "shader_manifest.json")
            for _, source, embedded in shader_embed.SHADERS:
                shutil.copy2(shader_embed.GPU_DIR / source, gpu_dir / source)
                shutil.copy2(shader_embed.GPU_DIR / embedded, gpu_dir / embedded)

            source = gpu_dir / "shaders" / "psp.frag"
            source.write_text(source.read_text(encoding="utf-8") + "\n// stale test\n")
            errors = shader_embed.verify(gpu_dir, gpu_dir / "shader_manifest.json")
            self.assertTrue(any("source SHA-256 mismatch" in error for error in errors), errors)

            shutil.copy2(shader_embed.GPU_DIR / "shaders" / "psp.frag", source)
            embedded = gpu_dir / "psp_vert.inc"
            embedded.write_text(embedded.read_text(encoding="ascii").replace("0x", "0X", 1))
            errors = shader_embed.verify(gpu_dir, gpu_dir / "shader_manifest.json")
            self.assertTrue(errors)

    def test_embedding_format_round_trips_spirv_bytes(self) -> None:
        data = bytes.fromhex("0302230700000100")
        self.assertEqual(shader_embed.parse_embedding(shader_embed.format_embedding(data)), data)


if __name__ == "__main__":
    unittest.main()
