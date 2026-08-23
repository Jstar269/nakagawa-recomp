# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regressions for the source-owned full-production smoke guest."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
GENERATOR_PATH = ROOT / "fixtures" / "production_smoke" / "generate.py"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import analyze  # noqa: E402
import imports as imports_tool  # noqa: E402
import prxload  # noqa: E402


SPEC = importlib.util.spec_from_file_location("production_smoke_generator", GENERATOR_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load {GENERATOR_PATH}")
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)


EXPECTED_PRX_SHA256 = "0e70188438318b1dd7324d9d08237634b4cb9f42b0078b189f72c569df9d9ace"
EXPECTED_PSP_SHA256 = "835e63d84cc41a67a868dd34d57b2cb39fdc153039f1c8c4dba781e54ae257e3"


class TestProductionSmoke(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="production_smoke_")
        self.addCleanup(self.temporary.cleanup)
        self.out_dir = Path(self.temporary.name)
        self.assertEqual(generator.generate(self.out_dir), 0)
        self.prx_path = self.out_dir / "guest.prx"
        self.psp_path = self.out_dir / "guest.psp"

    def test_generation_is_pinned_and_no_churn(self):
        prx = self.prx_path.read_bytes()
        psp = self.psp_path.read_bytes()
        manifest_path = self.out_dir / "manifest.json"
        before = {
            path.name: (path.read_bytes(), path.stat().st_mtime_ns)
            for path in (self.prx_path, self.psp_path, manifest_path)
        }
        self.assertEqual(generator.sha256(prx), EXPECTED_PRX_SHA256)
        self.assertEqual(generator.sha256(psp), EXPECTED_PSP_SHA256)
        self.assertEqual(generator.generate(self.out_dir), 0)
        after = {
            path.name: (path.read_bytes(), path.stat().st_mtime_ns)
            for path in (self.prx_path, self.psp_path, manifest_path)
        }
        self.assertEqual(after, before)
        manifest = json.loads(manifest_path.read_text(encoding="ascii"))
        self.assertEqual(manifest["kind"], "source-owned-psp-production-smoke")
        self.assertEqual(manifest["load_segments"], 2)
        self.assertEqual(manifest["relocation_count"], 10)
        self.assertEqual(manifest["bss_size"], 0x40)

    def test_real_loader_analyzer_and_import_parser_accept_fixture(self):
        loaded = prxload.Prx(self.prx_path, generator.BASE, psp_header=self.psp_path)
        load_segments = [segment for segment in loaded.segments if segment["type"] == 1]
        self.assertEqual(len(load_segments), 2)
        self.assertEqual(loaded.psp_bss_size, 0x40)
        self.assertEqual(loaded.relocate(), 10)
        self.assertEqual(len(loaded.mem), 0x10B0)
        pointer_offset = generator.RESULT_POINTER - generator.BASE
        result_offset = generator.RESULT - generator.BASE
        self.assertEqual(struct.unpack_from("<I", loaded.mem, pointer_offset)[0], generator.RESULT)
        self.assertEqual(struct.unpack_from("<I", loaded.mem, result_offset)[0], 0)
        self.assertEqual(loaded.mem[-0x40:], b"\0" * 0x40)

        elf = analyze.Elf(self.prx_path, base=generator.BASE)
        starts, ranges = analyze.analyze(elf)
        self.assertEqual(
            starts,
            {generator.ENTRY, generator.HELPER, generator.IMPORT_STUB},
        )
        self.assertEqual(
            ranges,
            [
                (generator.BASE, generator.BASE + 0x48),
                (generator.IMPORT_STUB, generator.IMPORT_STUB + 8),
            ],
        )
        self.assertEqual(
            imports_tool.parse_imports(elf),
            {generator.IMPORT_STUB: (generator.LIBRARY, generator.NID)},
        )

    def test_result_pointer_relocation_is_load_bearing(self):
        mutated = bytearray(self.prx_path.read_bytes())
        record_index = len(generator.relocation_records()) - 1
        record_offset = generator.RELOCATION_FILE_OFFSET + record_index * 8
        offset, info = struct.unpack_from("<II", mutated, record_offset)
        self.assertEqual(offset, 0x68)
        self.assertEqual(info & 0xF, generator.R_MIPS_32)
        struct.pack_into("<II", mutated, record_offset, offset, info & ~0xF)
        mutated_path = self.out_dir / "guest-no-result-relocation.prx"
        mutated_path.write_bytes(mutated)

        loaded = prxload.Prx(mutated_path, generator.BASE, psp_header=self.psp_path)
        self.assertEqual(loaded.relocate(), 10)
        pointer_offset = generator.RESULT_POINTER - generator.BASE
        self.assertEqual(struct.unpack_from("<I", loaded.mem, pointer_offset)[0], 0x6C)
        self.assertNotEqual(struct.unpack_from("<I", loaded.mem, pointer_offset)[0], generator.RESULT)

    def test_build_and_ci_route_use_the_production_target(self):
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        self.assertIn("production-smoke:", makefile)
        production_recipe = makefile.split("production-smoke:\n", 1)[1].split(
            "production-smoke-clean:", 1
        )[0]
        self.assertIn("$(MAKE) all", production_recipe)
        self.assertIn("GAME_PSP_HEADER=$(PRODUCTION_SMOKE_PSP)", production_recipe)
        self.assertIn("FUNCS_PER_CHUNK=1 PUBLIC_SAFE=1", production_recipe)
        self.assertNotIn("gate_stub", production_recipe)
        self.assertIn("mingw32-make --no-print-directory", workflow)
        self.assertIn("production-smoke", workflow)
        smoke_step = workflow.split(
            "- name: Build and run full production pipeline smoke", 1
        )[1].split("- name: Build and run scheduler selftest", 1)[0]
        self.assertIn("shell: msys2 {0}", smoke_step)
        self.assertIn("MSYS2_PATH_TYPE: inherit", smoke_step)
        self.assertIn("command -v pwsh", smoke_step)
        self.assertIn(
            "mingw32-make --no-print-directory CC=gcc VULKAN_SDK=/ucrt64 production-smoke",
            smoke_step,
        )


if __name__ == "__main__":
    unittest.main()
