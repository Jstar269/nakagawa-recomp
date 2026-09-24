# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regressions for the source-owned full-production smoke guest."""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
GENERATOR_PATH = ROOT / "fixtures" / "production_smoke" / "generate.py"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import analyze  # noqa: E402
import imports as imports_tool  # noqa: E402
import nk_cli  # noqa: E402
import prxload  # noqa: E402
import title_codegen_plan  # noqa: E402
from test_iso_parity import (  # noqa: E402
    build_plain_mips_elf,
    build_psp_container,
    create_test_iso_with_modules,
    create_test_iso_with_executables,
)
from test_import_name_safety import build_synthetic_import_prx  # noqa: E402


SPEC = importlib.util.spec_from_file_location("production_smoke_generator", GENERATOR_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load {GENERATOR_PATH}")
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)


EXPECTED_PRX_SHA256 = "0e70188438318b1dd7324d9d08237634b4cb9f42b0078b189f72c569df9d9ace"
EXPECTED_PSP_SHA256 = "835e63d84cc41a67a868dd34d57b2cb39fdc153039f1c8c4dba781e54ae257e3"
GAP_EXPECTED_PRX_SHA256 = "065cfc9092448d5689c922482e1b56d25b2abf56e52568c9582baea7f72f74c4"


def build_synthetic_cfw_loader(library: bytes = b"SystemCtrlForKernel") -> bytes:
    """Build a small source-owned executable with one CFW-only import."""
    executable, _stub = build_synthetic_import_prx(library, 0)
    executable = bytearray(executable)
    struct.pack_into("<H", executable, 16, 2)  # ordinary ET_EXEC EBOOT
    struct.pack_into("<H", executable, 50, 0)  # no section-name table
    shoff = struct.unpack_from("<I", executable, 32)[0]
    shentsize, shnum = struct.unpack_from("<HH", executable, 46)
    for index in range(shnum):
        struct.pack_into("<I", executable, shoff + index * shentsize, 0)
    phoff = struct.unpack_from("<I", executable, 28)[0]
    struct.pack_into("<I", executable, phoff + 28, 1)  # valid for file offset 0x100
    module_info = 0x100 + 0x40
    module_name = b"SyntheticLoader"
    executable[module_info + 4:module_info + 4 + len(module_name)] = module_name
    struct.pack_into("<I", executable, module_info + 32, 4)
    return bytes(executable)


def build_synthetic_original_elf() -> bytes:
    """Build a linked source-owned MIPS ELF suitable for an experimental profile."""
    executable, _stub = build_synthetic_import_prx(b"sceSynthetic", 0x08804000)
    executable = bytearray(executable)
    struct.pack_into("<H", executable, 16, 2)
    struct.pack_into("<I", executable, 24, 0x08804000)
    phoff = struct.unpack_from("<I", executable, 28)[0]
    struct.pack_into("<II", executable, phoff + 8, 0x08804000, 0x08804000)
    struct.pack_into("<I", executable, phoff + 28, 0x1000)
    shoff = struct.unpack_from("<I", executable, 32)[0]
    shentsize, shnum = struct.unpack_from("<HH", executable, 46)
    for section_index in range(1, shnum - 1):
        address_offset = shoff + section_index * shentsize + 12
        section_address = struct.unpack_from("<I", executable, address_offset)[0]
        struct.pack_into("<I", executable, address_offset, 0x08804000 + section_address)
    struct.pack_into("<H", executable, 50, 0)  # no section-name table
    for section_index in range(shnum):
        struct.pack_into("<I", executable, shoff + section_index * shentsize, 0)
    module_info = 0x100 + 0x40
    module_name = b"SyntheticOriginal"
    executable[module_info + 4:module_info + 4 + len(module_name)] = module_name
    struct.pack_into("<I", executable, module_info + 32, 4)
    return bytes(executable)


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
        self.assertEqual(manifest["mode"], "aot")
        self.assertEqual(manifest["load_segments"], 2)
        self.assertEqual(manifest["relocation_count"], 10)
        self.assertEqual(manifest["bss_size"], 0x40)

    def test_gap_fixture_is_pinned_and_keeps_guest_bytes(self):
        gap_dir = self.out_dir / "gap"
        self.assertEqual(generator.generate(gap_dir, mode="aot-gap"), 0)
        prx = (gap_dir / "guest.prx").read_bytes()
        self.assertEqual(generator.sha256(prx), GAP_EXPECTED_PRX_SHA256)
        self.assertEqual(
            generator.sha256((gap_dir / "guest.psp").read_bytes()), EXPECTED_PSP_SHA256
        )
        # The omitted region keeps its full body in the guest IMAGE bytes: an
        # AOT gap is an emission choice, never a byte removal.
        raw_helper = generator.build_text_segment("aot-gap")[0x28:0x80]
        self.assertIn(struct.pack("<I", 0x08000016), raw_helper)  # j REGION_B
        self.assertEqual(struct.unpack_from("<I", raw_helper, 0x08)[0], 0x24091234)
        self.assertEqual(struct.unpack_from("<I", raw_helper, 0x0C)[0], 0xAD090000)
        self.assertEqual(struct.unpack_from("<I", raw_helper, 0x10)[0], 0x8D020000)
        self.assertEqual(struct.unpack_from("<I", raw_helper, 0x18)[0], 0x24420001)
        manifest = json.loads((gap_dir / "manifest.json").read_text(encoding="ascii"))
        self.assertEqual(manifest["relocation_count"], 12)

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

    def test_gap_mode_discovers_but_can_omit_the_seam_region(self):
        """Analyzer still discovers the helper; codegen omission is emission-only."""
        gap_dir = self.out_dir / "gap"
        self.assertEqual(generator.generate(gap_dir, mode="aot-gap"), 0)
        elf = analyze.Elf(gap_dir / "guest.prx", base=generator.BASE)
        starts, _ = analyze.analyze(elf)
        self.assertIn(generator.HELPER, starts)
        self.assertIn(generator.REGION_B, starts)

        plain = subprocess.run(
            [sys.executable, str(TOOLS / "codegen.py"),
             str(gap_dir / "guest.prx"), str(self.out_dir / "plain.c"),
             f"--base={generator.BASE:#010x}", "--funcs-per-chunk=1"],
            capture_output=True, text=True, cwd=ROOT,
        )
        self.assertEqual(plain.returncode, 0, plain.stderr)
        chunks = sorted((self.out_dir).glob("plain_[0-9]*.c"))
        plain_text = (self.out_dir / "plain.c").read_text(encoding="ascii") + "\n".join(
            p.read_text(encoding="ascii") for p in chunks
        )
        self.assertIn(f"f_{generator.HELPER:08x}(", plain_text)

        omitted = subprocess.run(
            [sys.executable, str(TOOLS / "codegen.py"),
             str(gap_dir / "guest.prx"), str(self.out_dir / "omitted.c"),
             f"--base={generator.BASE:#010x}", "--funcs-per-chunk=1",
             f"--omit-aot=0x{generator.HELPER:08x}"],
            capture_output=True, text=True, cwd=ROOT,
        )
        self.assertEqual(omitted.returncode, 0, omitted.stderr)
        self.assertIn(f"CODEGEN_OMIT_AOT 0x{generator.HELPER:08x}", omitted.stderr)
        chunks = sorted((self.out_dir).glob("omitted_[0-9]*.c"))
        omitted_text = (self.out_dir / "omitted.c").read_text(encoding="ascii") + "\n".join(
            p.read_text(encoding="ascii") for p in chunks
        )
        # The seam: control leaves the compiled destination set through the
        # typed production dispatcher, targeting the omitted guest address with
        # the native continuation kept separate from $ra.
        self.assertIn(
            f"dispatch_call(s, 0x{generator.HELPER:08x}u, "
            f"0x{generator.ENTRY + 0x10:08x}u);",
            omitted_text,
        )
        self.assertNotIn(f"f_{generator.HELPER:08x}(", omitted_text)
        self.assertIn(f"f_{generator.REGION_B:08x}(", omitted_text)
        self.assertIn(
            f"sr_exec_span_register(0x{generator.BASE:08x}u, "
            f"0x{generator.IMPORT_STUB + 8:08x}u)",
            omitted_text,
        )
        self.assertEqual(omitted_text.count("sr_exec_span_register("), 1)
        self.assertNotIn(f"sr_exec_span_register(0x{generator.DATA_BASE:08x}u", omitted_text)

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

    def test_unknown_run_mode_is_refused(self):
        with self.assertRaises(RuntimeError):
            generator.run(self.out_dir, mode="does-not-exist")

    def test_gap_checker_rejects_every_look_alike(self):
        """Acceptance requires transfer, interpretation, AOT resume, and final state."""
        good = (
            f"DISPATCH 0x{generator.HELPER:08x} from 0x{generator.ENTRY:08x} "
            f"(ra=0x{generator.ENTRY + 0x10:08x})\n"
            f"GUEST_INTERP_ENTER entry=0x{generator.HELPER:08x} "
            f"caller_pc=0x{generator.ENTRY:08x} ra=0x{generator.ENTRY + 0x10:08x}\n"
            f"GUEST_INTERP_AOT_HANDOFF pc=0x{generator.REGION_B:08x} instructions=7\n"
            f"DISPATCH 0x{generator.REGION_B:08x} from 0x{generator.REGION_B:08x}\n"
            f"HLE: calling sceKernelSetCompiledSdkVersion (0x{generator.NID:08x})\n"
            f"DRIVER_EXPECT_U32 addr=0x{generator.RESULT:08x} "
            f"got=0x{generator.INTERP_RESULT:08x} "
            f"expected=0x{generator.INTERP_RESULT:08x} status=PASS\n"
        )
        generator.assert_gap_runtime_evidence(good, returncode=0)

        mutants = {
            "omission-removed": good.split("DISPATCH", 1)[0] + good.split("HLE:", 1)[1],
            "wrong-helper-target": good.replace(
                f"DISPATCH 0x{generator.HELPER:08x}",
                f"DISPATCH 0x{generator.REGION_B:08x}",
                1,
            ),
            "no-interpreter-transfer": good.replace("GUEST_INTERP_AOT_HANDOFF", "NO_TRANSFER"),
            "no-aot-region-b": good.replace(
                f"DISPATCH 0x{generator.REGION_B:08x} from 0x{generator.REGION_B:08x}",
                "AOT_REGION_B_SKIPPED",
            ),
            "old-fatal-miss": good + "NONPLT_MISS\n",
            "delay-slot-skipped": good.replace(
                f"got=0x{generator.INTERP_RESULT:08x} expected=0x{generator.INTERP_RESULT:08x} status=PASS",
                f"got=0x{generator.INTERP_STORE:08x} expected=0x{generator.INTERP_RESULT:08x} status=FAIL",
            ),
        }
        for name, log in mutants.items():
            with self.subTest(name=name), self.assertRaises(RuntimeError):
                generator.assert_gap_runtime_evidence(log, returncode=1 if name == "old-fatal-miss" else 0)

    def _fabricated_aot_tree(self, root: Path, relative_dir: str) -> Path:
        """Minimal build tree satisfying verify(--mode aot) with RELATIVE-spelled
        link-map entries, for the relative/absolute --build-dir contract test."""
        build_dir = root / relative_dir
        build_dir.mkdir(parents=True, exist_ok=True)
        self.addCleanup(shutil.rmtree, build_dir, ignore_errors=True)
        fixture = build_dir / "fixture"
        fixture.mkdir()
        self.assertEqual(generator.generate(fixture), 0)

        image = bytearray(0x10B0)
        struct.pack_into("<I", image, generator.RESULT_POINTER - generator.BASE, generator.RESULT)
        helper_bytes = generator.expected_helper_bytes("aot")
        image[generator.HELPER - generator.BASE:generator.HELPER - generator.BASE + len(helper_bytes)] = helper_bytes
        (build_dir / "production_smoke_image.bin").write_bytes(bytes(image))
        (build_dir / "production_smoke.exe").write_bytes(b"MZ-fake")

        (build_dir / "production_smoke_recomp.c").write_text(
            f"sr_exec_span_register(0x{generator.BASE:08x}u, "
            f"0x{generator.BASE + generator.TEXT_SECTION_SIZE_AOT:08x}u);\n"
            f"sr_exec_span_register(0x{generator.IMPORT_STUB:08x}u, "
            f"0x{generator.IMPORT_STUB + 8:08x}u);\n"
            'fprintf(stderr, "sr_register_all: registered 2 executable span(s)\\n");\n'
            'fprintf(stderr, "sr_register_all: starting 3 registrations\\n");\n'
            "sr_register_chunk_0();\nsr_register_chunk_1();\nsr_register_chunk_2();\n",
            encoding="ascii",
        )
        (build_dir / "production_smoke_recomp_funcs.h").write_text(
            "/* fabricated */\n", encoding="ascii"
        )
        generated = (
            f"void f_{generator.ENTRY:08x}(CpuState *s) {{}}\n"
            f"void f_{generator.HELPER:08x}(CpuState *s) {{}}\n"
            f"void f_{generator.IMPORT_STUB:08x}(CpuState *s) {{}}\n"
            f"sr_syscall(s, 0x{generator.NID:08x}u);\n"
        )
        for index in range(3):
            (build_dir / f"production_smoke_recomp_{index}.c").write_text(generated, encoding="ascii")
            (build_dir / f"production_smoke_recomp_{index}.o").write_bytes(b"\0")
        (build_dir / "production_smoke_imports.toml").write_text(
            f'{generator.LIBRARY} = ["0x{generator.NID:08x}"]\n', encoding="ascii"
        )

        required = [
            "production_smoke_recomp.o",
            "production_smoke_recomp_0.o",
            "production_smoke_recomp_1.o",
            "production_smoke_recomp_2.o",
            "ge.o", "flight_recorder.o", "recomp.o", "guest_interp.o", "title_config.o", "vfpu_tables.o", "debug.o",
            "watchpoints_file.o", "guest_printf.o", "perf.o", "fbcap_policy.o",
            "ge_capture.o", "vfpu_interp.o", "hle.o", "sched.o", "sr_coro.o",
            "iso_public.o", "pgd_unavailable.o", "mpeg.o", "pgf_unavailable.o",
            "gui.o", "audio_unavailable.o", "h264_mf.o", "h264_null.o", "savedata.o",
            "osk_win.o", "driver.o", "sdl3vk.o", "ge_gpu.o",
            "atrac3p_atrac3p_api.o",
            "atrac3p_libavcodec/atrac.o", "atrac3p_libavcodec/atrac3plus.o",
            "atrac3p_libavcodec/atrac3plusdec.o", "atrac3p_libavcodec/atrac3plusdsp.o",
            "atrac3p_libavcodec/bitstream.o", "atrac3p_libavcodec/fft_float.o",
            "atrac3p_libavcodec/fft_init_table.o", "atrac3p_libavcodec/mdct_float.o",
            "atrac3p_libavcodec/sinewin.o", "atrac3p_libavutil/float_dsp.o",
            "atrac3p_libavutil/intmath.o", "atrac3p_libavutil/log2_tab.o",
            "atrac3p_libavutil/mem.o", "atrac3p_libavutil/reverse.o",
            "atrac3p_bridge.o",
        ]
        # Map spelled RELATIVE to the process cwd, exactly like the production
        # link produces when BUILD_DIR is relative.
        (build_dir / "production_smoke.map").write_text(
            "\n".join(f"{relative_dir}/{name}" for name in required) + "\n",
            encoding="utf-8",
        )
        return build_dir

    def test_verify_accepts_relative_and_absolute_build_dir_spellings(self):
        with tempfile.TemporaryDirectory(prefix="nk_prod_smoke_") as tmp_dir:
            temp_root = Path(tmp_dir)
            relative_dir = "sub_build"
            build_dir = self._fabricated_aot_tree(temp_root, relative_dir)
            cwd = os.getcwd()
            try:
                os.chdir(temp_root)
                # Relative spelling.
                generator.verify(Path(relative_dir), mode="aot")
                # Semantically identical absolute spelling of the SAME directory.
                generator.verify(build_dir.resolve(), mode="aot")
            finally:
                os.chdir(cwd)

    def test_build_and_ci_route_use_the_production_targets(self):
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
        gap_recipe = makefile.split("production-smoke-gap:\n", 1)[1].split(
            "production-smoke-gap-clean:", 1
        )[0]
        self.assertIn("--mode aot-gap", gap_recipe)
        self.assertIn("CODEGEN_USER_ARGS=$(PRODUCTION_SMOKE_GAP_CODEGEN_ARGS)", gap_recipe)
        self.assertIn("PRODUCTION_SMOKE_GAP_CODEGEN_ARGS := --omit-aot=", makefile)
        self.assertIn("mingw32-make --no-print-directory", workflow)
        smoke_step = workflow.split(
            "- name: Build and run full production pipeline smoke", 1
        )[1].split("- name:", 1)[0]
        self.assertIn("shell: msys2 {0}", smoke_step)
        self.assertIn("MSYS2_PATH_TYPE: inherit", smoke_step)
        self.assertIn("command -v pwsh", smoke_step)
        self.assertIn(
            "mingw32-make --no-print-directory CC=gcc VULKAN_SDK=/ucrt64 production-smoke",
            smoke_step,
        )
        gap_step = workflow.split(
            "- name: Build and run AOT-gap dispatch-seam smoke", 1
        )[1].split("- name:", 1)[0]
        self.assertIn("MSYS2_PATH_TYPE: inherit", gap_step)
        self.assertIn(
            "mingw32-make --no-print-directory CC=gcc VULKAN_SDK=/ucrt64 production-smoke-gap",
            gap_step,
        )


class TestProductionSmokePackage(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="nk-package-smoke-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build_dir = self.root / "package"
        self.fixture_dir = self.build_dir / "fixture"
        self.assertEqual(generator.generate(self.fixture_dir), 0)
        self.manifest_path = self.root / "title.json"
        self.manifest = {
            "schema_version": 1,
            # The generated bytes remain the source-owned smoke fixture. The
            # synthetic library identity exercises the same experimental
            # profile binding used by an imported uncatalogued disc.
            "id": "experimental-ulus99998",
            "display_name": "Synthetic production smoke package",
            "kind": "retail",
            "disc": {
                "id": "ULUS99998",
                "region": "NA",
                "revision_policy": "exact-disc-id",
            },
            "game_name": "production_smoke",
            "executable": {
                "base": generator.BASE,
                "entry": generator.ENTRY,
                "bss_metadata_source": "psp-header",
                "extra_executable_spans": [],
            },
            "modules": [],
            "filesystem": {
                "data_root": "fixtures/production_smoke/data",
                "memory_stick_root": "build/production_smoke/memstick",
                "device_prefixes": ["host0:", "ms0:"],
            },
            "hle_profile": "synthetic-minimal",
            "codegen_profile": "none",
            "feature_requirements": ["allegrex-core"],
            "verification_profile": "synthetic-public",
        }
        self.manifest_path.write_text(
            json.dumps(self.manifest, sort_keys=True) + "\n", encoding="utf-8"
        )

    def skip_if_toolchain_unusable(self, run):
        """The package route links SDL3; a host without the supported toolchain is a
        SKIP with the Makefile's own remedy, not a failure of the route."""
        output = (run.stdout + run.stderr)
        if run.returncode != 0 and "sdl3 dependency is missing" in output.lower():
            reason = next(line for line in output.splitlines()
                          if "sdl3 dependency is missing" in line.lower())
            self.skipTest(reason.strip())

    def package_command(self, *, executable=None, output_dir=None):
        return [
            sys.executable,
            str(ROOT / "tools" / "title_codegen_plan.py"),
            str(self.manifest_path),
            "--package",
            "--game-elf",
            str(executable or (self.fixture_dir / "guest.prx")),
            "--psp-header",
            str(self.fixture_dir / "guest.psp"),
            "--output-dir",
            str(output_dir or self.build_dir),
            "--funcs-per-chunk",
            "1",
            "--public-safe",
        ]

    def test_translation_fallback_keeps_a_named_semantic_boundary(self):
        elf = analyze.Elf(self.fixture_dir / "guest.prx", base=generator.BASE)
        instruction_address = generator.HELPER + 4
        report_path = self.root / "synthetic-stubs.txt"
        report_path.write_text(
            f"0x{generator.HELPER:08x} opcode 0x3f at 0x{instruction_address:08x}\n",
            encoding="ascii",
        )
        regions, instructions = title_codegen_plan._read_codegen_fallbacks(
            report_path,
            [{"name": "guest.prx", "ranges": analyze.exec_ranges(elf), "elf": elf}],
        )
        self.assertEqual(regions[0]["boundary"], f"AOT function at 0x{generator.HELPER:08x}")
        self.assertEqual(regions[0]["status"], "in the works")
        self.assertEqual(regions[0]["tracking_issue"], 118)
        self.assertEqual(instructions[0]["address"], f"0x{instruction_address:08x}")
        self.assertTrue(instructions[0]["word"].startswith("0x"))
        self.assertEqual(instructions[0]["status"], "in the works")

    def test_make_unsafe_inputs_are_staged_not_refused(self):
        spaced = self.root / "My Games" / "disc input"
        spaced.mkdir(parents=True)
        source = spaced / "guest file.prx"
        source.write_bytes(b"synthetic bytes")
        stage = self.build_dir / "staged-inputs"
        staged = title_codegen_plan._stage_for_make(source, stage, "executable ELF")
        self.assertEqual(staged.parent, stage)
        self.assertFalse(title_codegen_plan._make_unsafe(staged.as_posix()))
        self.assertEqual(staged.read_bytes(), source.read_bytes())
        safe = self.fixture_dir / "guest.prx"
        self.assertEqual(title_codegen_plan._stage_for_make(safe, stage, "x"), safe)

    def test_make_unsafe_output_directory_is_refused_with_remedy(self):
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("NK_BUILD_ROOT", None)
            with mock.patch("title_codegen_plan._windows_short_path", return_value=None):
                with self.assertRaises(title_codegen_plan.PackageRouteError) as caught:
                    title_codegen_plan.build_package(
                        self.manifest,
                        manifest_path=self.manifest_path,
                        game_elf=self.fixture_dir / "guest.prx",
                        psp_header=self.fixture_dir / "guest.psp",
                        output_dir=self.root / "out dir",
                        public_safe=True,
                    )
                self.assertEqual(caught.exception.code, "PACKAGE_UNSUPPORTED_PATH")
                self.assertIn("contains spaces", str(caught.exception))
                self.assertIn("8.3 short names are unavailable on this volume", str(caught.exception))
                self.assertIn("set NK_BUILD_ROOT to a folder without spaces", str(caught.exception))

    def test_make_unsafe_characters_output_directory_is_refused(self):
        with self.assertRaises(title_codegen_plan.PackageRouteError) as caught:
            title_codegen_plan.build_package(
                self.manifest,
                manifest_path=self.manifest_path,
                game_elf=self.fixture_dir / "guest.prx",
                psp_header=self.fixture_dir / "guest.psp",
                output_dir=self.root / "out#dir",
                public_safe=True,
            )
        self.assertEqual(caught.exception.code, "PACKAGE_UNSUPPORTED_PATH")
        self.assertIn("characters the Make recipes cannot quote", str(caught.exception))

    def test_package_builds_to_spaced_destination_via_nk_build_root(self):
        required = ("mingw32-make", "gcc", "pwsh")
        if not all(shutil.which(name) for name in required):
            self.skipTest("the production package route requires mingw32-make, gcc, and pwsh")
        safe_build_root = self.root / "safe_build_root"
        safe_build_root.mkdir(parents=True)
        spaced_dest = self.root / "Spaced Destination Root"
        with mock.patch.dict(os.environ, {"NK_BUILD_ROOT": str(safe_build_root)}):
            title_codegen_plan.build_package(
                self.manifest,
                manifest_path=self.manifest_path,
                game_elf=self.fixture_dir / "guest.prx",
                psp_header=self.fixture_dir / "guest.psp",
                output_dir=spaced_dest,
                public_safe=True,
            )
        self.assertTrue((spaced_dest / "package.json").is_file())
        self.assertTrue((spaced_dest / "build-report.json").is_file())
        self.assertTrue((spaced_dest / "production_smoke.exe").is_file())
        package_text = (spaced_dest / "package.json").read_text(encoding="utf-8")
        report_text = (spaced_dest / "build-report.json").read_text(encoding="utf-8")
        self.assertNotIn(str(safe_build_root), package_text)
        self.assertNotIn(".build-", package_text)
        self.assertNotIn(str(safe_build_root), report_text)
        self.assertNotIn(".build-", report_text)
        scratch_dirs = list(safe_build_root.glob(".build-*"))
        self.assertEqual(len(scratch_dirs), 0)

    def test_package_builds_to_spaced_destination_via_short_path_provider(self):
        required = ("mingw32-make", "gcc", "pwsh")
        if not all(shutil.which(name) for name in required):
            self.skipTest("the production package route requires mingw32-make, gcc, and pwsh")
        safe_short_root = self.root / "SHORTP~1"
        safe_short_root.mkdir(parents=True)
        spaced_dest = self.root / "Spaced Destination Short"
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("NK_BUILD_ROOT", None)
            with mock.patch("title_codegen_plan._windows_short_path", return_value=safe_short_root):
                title_codegen_plan.build_package(
                    self.manifest,
                    manifest_path=self.manifest_path,
                    game_elf=self.fixture_dir / "guest.prx",
                    psp_header=self.fixture_dir / "guest.psp",
                    output_dir=spaced_dest,
                    public_safe=True,
                )
        self.assertTrue((spaced_dest / "package.json").is_file())
        self.assertTrue((spaced_dest / "build-report.json").is_file())
        self.assertTrue((spaced_dest / "production_smoke.exe").is_file())
        package_text = (spaced_dest / "package.json").read_text(encoding="utf-8")
        report_text = (spaced_dest / "build-report.json").read_text(encoding="utf-8")
        self.assertNotIn(str(safe_short_root), package_text)
        self.assertNotIn(".build-", package_text)
        self.assertNotIn(str(safe_short_root), report_text)
        self.assertNotIn(".build-", report_text)
        scratch_dirs = list(safe_short_root.glob(".build-*"))
        self.assertEqual(len(scratch_dirs), 0)

    def test_package_builds_to_real_spaced_destination_without_mocks(self):
        # Unmocked: the real 8.3 alias must survive (Path.resolve() would expand it back).
        required = ("mingw32-make", "gcc", "pwsh")
        if os.name != "nt" or not all(shutil.which(name) for name in required):
            self.skipTest("real 8.3 short-path route needs Windows and the native toolchain")
        spaced_parent = self.root / "Real Spaced Parent"
        spaced_parent.mkdir(parents=True)
        short = title_codegen_plan._windows_short_path(spaced_parent)
        if short is None or title_codegen_plan._make_unsafe(short.as_posix()):
            self.skipTest("8.3 short names are unavailable on this volume")
        spaced_dest = spaced_parent / "pkg"
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("NK_BUILD_ROOT", None)
            title_codegen_plan.build_package(
                self.manifest,
                manifest_path=self.manifest_path,
                game_elf=self.fixture_dir / "guest.prx",
                psp_header=self.fixture_dir / "guest.psp",
                output_dir=spaced_dest,
                public_safe=True,
            )
        self.assertTrue((spaced_dest / "package.json").is_file())
        self.assertTrue((spaced_dest / "production_smoke.exe").is_file())
        self.assertNotIn(".build-", (spaced_dest / "package.json").read_text(encoding="utf-8"))

    def test_promotion_failure_leaves_no_partial_package(self):
        ws = self.root / "mock_ws"
        ws.mkdir()
        (ws / "package.json").write_text('{"mock": true}', encoding="utf-8")
        (ws / "production_smoke.exe").write_bytes(b"mock exe")

        # Test case A: destination did not exist prior to promotion
        spaced_dest = self.root / "Promo Fail Destination"
        with mock.patch("os.replace", side_effect=OSError("simulated disk full during atomic replace")):
            with self.assertRaises(OSError):
                title_codegen_plan._promote_package(ws, spaced_dest)
        self.assertFalse(spaced_dest.exists())
        stagings = list(self.root.glob(f".{spaced_dest.name}.staging-*"))
        self.assertEqual(len(stagings), 0)

        # Test case B: destination existed prior to promotion and should be restored
        spaced_dest_existing = self.root / "Promo Restore Destination"
        spaced_dest_existing.mkdir()
        (spaced_dest_existing / "original.txt").write_text("original content", encoding="utf-8")
        original_replace = os.replace
        def failing_replace_staging(src, dst):
            if str(spaced_dest_existing) in str(dst) and ".staging-" in str(src):
                raise OSError("simulated failure replacing target")
            return original_replace(src, dst)
        with mock.patch("os.replace", side_effect=failing_replace_staging):
            with self.assertRaises(OSError):
                title_codegen_plan._promote_package(ws, spaced_dest_existing)
        self.assertTrue(spaced_dest_existing.exists())
        self.assertTrue((spaced_dest_existing / "original.txt").is_file())
        self.assertFalse((spaced_dest_existing / "production_smoke.exe").exists())
        stagings = list(self.root.glob(f".{spaced_dest_existing.name}.staging-*"))
        backups = list(self.root.glob(f".{spaced_dest_existing.name}.backup-*"))
        self.assertEqual(len(stagings), 0)
        self.assertEqual(len(backups), 0)

    def test_package_builds_from_inputs_under_a_path_with_spaces(self):
        required = ("mingw32-make", "gcc", "pwsh")
        if not all(shutil.which(name) for name in required):
            self.skipTest("the production package route requires mingw32-make, gcc, and pwsh")
        spaced = self.root / "User Name" / "Downloads"
        spaced.mkdir(parents=True)
        executable = spaced / "guest.prx"
        shutil.copyfile(self.fixture_dir / "guest.prx", executable)
        command = self.package_command(executable=executable)
        header_at = command.index("--psp-header") + 1
        shutil.copyfile(self.fixture_dir / "guest.psp", spaced / "guest.psp")
        command[header_at] = str(spaced / "guest.psp")
        run = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        self.skip_if_toolchain_unusable(run)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        package = json.loads((self.build_dir / "package.json").read_bytes())
        self.assertEqual(
            package["inputs"]["executable"]["sha256"],
            hashlib.sha256(executable.read_bytes()).hexdigest(),
        )

    def test_package_action_runs_production_smoke_and_emits_deterministic_contract(self):
        required = ("mingw32-make", "gcc", "pwsh")
        if not all(shutil.which(name) for name in required):
            self.skipTest("the production package route requires mingw32-make, gcc, and pwsh")

        env = os.environ.copy()
        first = subprocess.run(
            self.package_command(), cwd=ROOT, env=env, capture_output=True, text=True
        )
        self.skip_if_toolchain_unusable(first)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        package_path = self.build_dir / "package.json"
        report_path = self.build_dir / "build-report.json"
        package_bytes = package_path.read_bytes()
        report_bytes = report_path.read_bytes()
        package = json.loads(package_bytes)
        report = json.loads(report_bytes)

        self.assertEqual(package["format"], "nakagawa-aot-package")
        self.assertEqual(package["schema_version"], 1)
        self.assertEqual(package["title"]["id"], self.manifest["id"])
        self.assertEqual(package["runtime"]["abi"], "CpuState")
        self.assertEqual(package["runtime"]["abi_version"], 2)
        self.assertEqual(package["executable"]["path"], "production_smoke.exe")
        self.assertTrue((self.build_dir / package["executable"]["path"]).is_file())
        self.assertEqual(
            package["inputs"]["executable"]["sha256"],
            hashlib.sha256((self.fixture_dir / "guest.prx").read_bytes()).hexdigest(),
        )
        self.assertTrue(package["generated_objects"])
        for obj in package["generated_objects"]:
            self.assertTrue(obj["path"].endswith(".o"))
            self.assertTrue((self.build_dir / obj["path"]).is_file())
        self.assertTrue(package["required_local_assets"])

        self.assertEqual(report["format"], "nakagawa-build-report")
        self.assertEqual(report["schema_version"], 1)
        self.assertTrue(report["tools"]["compiler"]["identity"])
        for field in (
            "analyzed_functions",
            "aot_entries",
            "fallback_entries",
            "unsupported_import_count",
        ):
            self.assertIsInstance(report["coverage"][field], int)
        self.assertEqual(
            set(report["unsupported"]), {"imports", "instructions", "regions"}
        )

        second = subprocess.run(
            self.package_command(), cwd=ROOT, env=env, capture_output=True, text=True
        )
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(package_path.read_bytes(), package_bytes)
        self.assertEqual(report_path.read_bytes(), report_bytes)
        self.assertEqual(generator.verify(self.build_dir, mode="aot"), 0)
        self.assertEqual(generator.run(self.build_dir, mode="aot"), 0)

        # End-to-end discovery: put the production-smoke package at the
        # per-user path, bind it to a synthetic experimental profile, and use
        # the native player validator for both the accepted and tampered copy.
        user_root = self.root / "player-user-data"
        package_dir = user_root / "packages" / "ULUS99998"
        shutil.copytree(self.build_dir, package_dir)
        executable_hash = hashlib.sha256((self.fixture_dir / "guest.prx").read_bytes()).hexdigest()
        profile_dir = user_root / "experimental" / "ULUS99998"
        profile_dir.mkdir(parents=True)
        profile = {
            "schema_version": 1,
            "manifest": self.manifest,
            "input_identity": {
                "disc_id": "ULUS99998",
                "selected_executable": "PSP_GAME/SYSDIR/EBOOT.BIN",
                "executable_sha256": executable_hash,
                "elf_sha256": executable_hash,
            },
        }
        (profile_dir / "profile.json").write_text(
            json.dumps(profile, sort_keys=True) + "\n", encoding="utf-8"
        )

        player_env = os.environ.copy()
        ucrt_bin = Path("C:/msys64/ucrt64/bin")
        if ucrt_bin.is_dir():
            player_env["PATH"] = str(ucrt_bin) + os.pathsep + player_env.get("PATH", "")
        native_build = subprocess.run(
            ["mingw32-make", "--no-print-directory", "player-state-test-bin"],
            cwd=ROOT, env=player_env, capture_output=True, text=True,
        )
        self.assertEqual(native_build.returncode, 0, native_build.stdout + native_build.stderr)
        validator = ROOT / "build" / "test_player_state.exe"
        args = [str(validator), "--validate-package", str(user_root),
                "ULUS99998", self.manifest["id"], "1", "EBOOT.BIN"]
        accepted = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(accepted.returncode, 0, accepted.stdout + accepted.stderr)
        self.assertIn("PACKAGE_STATUS=OK", accepted.stdout)

        executable_path = package_dir / package["executable"]["path"]
        executable_path.write_bytes(executable_path.read_bytes() + b"tampered")
        rejected = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(rejected.returncode, 0, rejected.stdout + rejected.stderr)
        self.assertIn("PACKAGE_STATUS=STALE", rejected.stdout)
        self.assertIn("executable hash is stale", rejected.stdout)

    def test_build_package_cli_refuses_encrypted_selected_executable(self):
        user_root = self.root / "cli-user-data"
        user_root.mkdir()
        iso_path = self.root / "synthetic.iso"
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
        command = [sys.executable, str(TOOLS / "nk_cli.py"), "build-package",
                   "ULUS99998", "--user-data-root", str(user_root)]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        # The refusal names the per-title folder for user-supplied decrypted inputs.
        self.assertIn("supply decrypted modules at", result.stderr)
        self.assertIn(str(Path("titles") / "ULUS99998" / "decrypted"), result.stderr)
        self.assertIn("(#295)", result.stderr)
        self.assertIn("in the works", result.stderr)
        self.assertFalse((user_root / "cache").exists())

    def test_library_cli_extracts_plaintext_elf_before_named_build_boundary(self):
        required = ("mingw32-make", "gcc", "pwsh")
        if not all(shutil.which(name) for name in required):
            self.skipTest("the production package route requires mingw32-make, gcc, and pwsh")
        if os.name == "nt" and not Path("C:/msys64/ucrt64/bin/SDL3.dll").is_file():
            self.skipTest("the player package route requires the SDL3 runtime DLL")

        user_root = self.root / "cli-user-data"
        user_root.mkdir()
        iso_path = self.root / "synthetic-plain-elf.iso"
        executable = bytearray((self.fixture_dir / "guest.prx").read_bytes())
        # The pinned smoke guest intentionally uses load-relative zero
        # addresses. Make its source-owned ELF satisfy the ISO preflight's
        # alignment congruence while preserving its code/data bytes.
        phoff = struct.unpack_from("<I", executable, 28)[0]
        phnum = struct.unpack_from("<H", executable, 44)[0]
        for index in range(phnum):
            struct.pack_into("<I", executable, phoff + index * 32 + 28, 0x100)
        executable_bytes = bytes(executable)
        create_test_iso_with_executables(
            iso_path, executable_bytes, disc_id="ULUS99998"
        )
        executable_hash = hashlib.sha256(executable_bytes).hexdigest()
        profile_dir = user_root / "experimental" / "ULUS99998"
        profile_dir.mkdir(parents=True)
        profile = {
            "schema_version": 1,
            "manifest": self.manifest,
            "input_identity": {
                "disc_id": "ULUS99998",
                "selected_executable": "PSP_GAME/SYSDIR/EBOOT.BIN",
                "executable_sha256": executable_hash,
                "elf_sha256": executable_hash,
            },
        }
        (profile_dir / "profile.json").write_text(
            json.dumps(profile, sort_keys=True) + "\n", encoding="utf-8"
        )
        library = {
            "schema_version": 1,
            "games": [{
                "disc_id": "ULUS99998",
                "title_id": self.manifest["id"],
                "iso_path": str(iso_path),
                "selected_executable": "EBOOT.BIN",
                "is_experimental": True,
            }],
        }
        (user_root / "library.json").write_text(
            json.dumps(library, sort_keys=True) + "\n", encoding="utf-8"
        )

        env = os.environ.copy()
        ucrt_bin = Path("C:/msys64/ucrt64/bin")
        if ucrt_bin.is_dir():
            env["PATH"] = str(ucrt_bin) + os.pathsep + env.get("PATH", "")
        command = [sys.executable, str(TOOLS / "nk_cli.py"), "build-package",
                   "ULUS99998", "--user-data-root", str(user_root),
                   "--psp-header", str(self.fixture_dir / "guest.psp")]
        completed = subprocess.run(command, cwd=ROOT, env=env,
                                   capture_output=True, text=True)
        self.skip_if_toolchain_unusable(completed)
        cached_elf = user_root / "cache" / "packages" / "ULUS99998" / "selected.elf"
        self.assertEqual(cached_elf.read_bytes(), executable_bytes)
        package_dir = user_root / "packages" / "ULUS99998"
        if completed.returncode == 0:
            self.assertTrue((package_dir / "package.json").is_file())
            return
        self.assertIn("production PGF/PGD runtime backends", completed.stderr)
        self.assertIn("in the works (#297)", completed.stderr)
        self.assertFalse((package_dir / "package.json").exists())

    def test_bad_executable_is_rejected_with_a_named_reason(self):
        bad_elf = self.root / "bad.elf"
        bad_elf.write_bytes(b"not-an-ELF")
        bad_output = self.root / "bad-package"
        completed = subprocess.run(
            self.package_command(executable=bad_elf, output_dir=bad_output),
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("PACKAGE_INVALID_ELF:", completed.stderr)
        self.assertFalse((bad_output / "package.json").exists())
        self.assertFalse((bad_output / "build-report.json").exists())


class TestSanitizedBringup(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="nk-bringup-report-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        executable, _stub = build_synthetic_import_prx(b"sceSynthetic", 0)
        executable = bytearray(executable)
        struct.pack_into("<H", executable, 16, 2)
        phoff = struct.unpack_from("<I", executable, 28)[0]
        struct.pack_into("<I", executable, phoff + 28, 0x100)
        self.iso = self.root / "synthetic.iso"
        create_test_iso_with_executables(
            self.iso, bytes(executable), disc_id="ULUS99998",
            title="Synthetic Bring-up",
        )

    def _run_case(self, failure=None, *, launch_code=0, launch_output="", timeout=False):
        case_root = self.root / (failure or "success")
        report_path = case_root / "bringup.json"
        args = argparse.Namespace(
            iso=str(self.iso),
            work_dir=str(case_root / "work"),
            report=str(report_path),
            launch_timeout=1,
        )
        globals_timeout = timeout
        launch_commands = []

        class FakeProcess:
            def __init__(self):
                self.returncode = launch_code

            def communicate(self, timeout=None):
                if timeout is not None and globals_timeout:
                    raise subprocess.TimeoutExpired("synthetic-runtime", timeout)
                return launch_output, None

            def kill(self):
                self.returncode = -9

        def fake_package_build(build_args, stage_observer=None):
            if failure == "compile":
                stage_observer("compile", "FAIL", 3)
                return 1
            if failure == "build_package":
                return 1
            stage_observer("compile", "PASS", 3)
            package_dir = build_args.user_data_root / "packages" / "ULUS99998"
            package_dir.mkdir(parents=True, exist_ok=True)
            (package_dir / "runtime.exe").write_bytes(b"synthetic runtime marker")
            (package_dir / "runtime_image.bin").write_bytes(b"synthetic runtime image")
            (package_dir / "package.json").write_text(
                json.dumps({
                    "executable": {"path": "runtime.exe"},
                    "runtime": {"run_entry": "0x08800100"},
                    "required_local_assets": [{"path": "data"}],
                }),
                encoding="utf-8",
            )
            stage_observer("build_package", "PASS", 2)
            return 0

        def fake_popen(command, **_kwargs):
            launch_commands.append(list(command))
            return FakeProcess()

        completed = subprocess.CompletedProcess(["synthetic-codegen"], 1 if failure == "codegen" else 0, "", "")
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(nk_cli.subprocess, "run", return_value=completed))
            stack.enter_context(mock.patch.object(
                nk_cli.subprocess, "Popen", side_effect=fake_popen
            ))
            stack.enter_context(mock.patch.object(
                nk_cli, "cmd_build_package", side_effect=fake_package_build
            ))
            if failure == "prepare_import":
                stack.enter_context(mock.patch.object(
                    nk_cli, "write_experimental_profile", side_effect=RuntimeError("synthetic refusal")
                ))
            if failure == "analyze":
                stack.enter_context(mock.patch.object(
                    title_codegen_plan, "_make_input_images", side_effect=RuntimeError("synthetic refusal")
                ))
            if failure == "inspect":
                broken_iso = case_root / "broken.iso"
                broken_iso.parent.mkdir(parents=True, exist_ok=True)
                broken_iso.write_bytes(b"invalid synthetic image")
                args.iso = str(broken_iso)
            with mock.patch("builtins.print"):
                status = nk_cli.cmd_bringup(args)
        self.last_launch_command = launch_commands[-1] if launch_commands else None
        return status, json.loads(report_path.read_text(encoding="utf-8"))

    def test_synthetic_consumer_stages_succeed_and_report_is_sanitized(self):
        status, report = self._run_case()
        self.assertEqual(status, 0, report)
        self.assertEqual(report["failure_class"], "NONE")
        self.assertEqual(report["reached_stage"], "launch")
        self.assertTrue(all(stage["status"] == "PASS" for stage in report["stages"].values()))
        self.assertGreater(report["counts"]["functions"], 0)
        self.assertGreater(report["counts"]["instructions"], 0)
        command = self.last_launch_command
        self.assertIsNotNone(command)
        self.assertEqual(command[1], "--image")
        self.assertTrue(command[2].endswith("runtime_image.bin"))
        self.assertRegex(command[3], r"^(?:0|0x[0-9a-f]{8})$")
        self.assertRegex(command[4], r"^0x[0-9a-f]{8}$")
        self.assertEqual(command[5:], ["none", "none", "--sched"])
        nk_cli.validate_bringup_report(report)

    def _run_module_fixture(
        self, iso_path: Path, work_root: Path, *,
        user_decrypted_eboot: bytes | None = None,
        forbid_iso_executable: bool = False,
    ):
        work_dir = work_root / "work"
        report_path = work_root / "bringup.json"
        if user_decrypted_eboot is not None:
            decrypted_dir = (
                work_dir / "user-data" / "titles" / "ULUS99998" / "decrypted"
            )
            decrypted_dir.mkdir(parents=True, exist_ok=True)
            (decrypted_dir / "EBOOT.elf").write_bytes(user_decrypted_eboot)
        args = argparse.Namespace(
            iso=str(iso_path),
            work_dir=str(work_dir),
            report=str(report_path),
            launch_timeout=1,
        )

        class FakeProcess:
            returncode = 0

            def communicate(self, timeout=None):
                return "", None

        def fake_package_build(build_args, stage_observer=None):
            stage_observer("compile", "PASS", 1)
            package_dir = build_args.user_data_root / "packages" / "ULUS99998"
            package_dir.mkdir(parents=True, exist_ok=True)
            (package_dir / "runtime.exe").write_bytes(b"synthetic runtime")
            (package_dir / "runtime_image.bin").write_bytes(b"synthetic runtime image")
            (package_dir / "package.json").write_text(
                json.dumps({
                    "executable": {"path": "runtime.exe"},
                    "runtime": {"run_entry": "0x08800100"},
                    "required_local_assets": [{"path": "data"}],
                }),
                encoding="utf-8",
            )
            stage_observer("build_package", "PASS", 1)
            return 0

        completed = subprocess.CompletedProcess(["synthetic-codegen"], 0, "", "")
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(
                nk_cli.subprocess, "run", return_value=completed
            ))
            stack.enter_context(mock.patch.object(
                nk_cli.subprocess, "Popen", return_value=FakeProcess()
            ))
            stack.enter_context(mock.patch.object(
                nk_cli, "cmd_build_package", side_effect=fake_package_build
            ))
            if forbid_iso_executable:
                stack.enter_context(mock.patch.object(
                    nk_cli, "_extract_iso_executable",
                    side_effect=AssertionError("CFW loader was selected for analysis"),
                ))
            with mock.patch("builtins.print"):
                return nk_cli.cmd_bringup(args), json.loads(
                    report_path.read_text(encoding="utf-8")
                )

    def test_multi_module_iso_gets_provisional_non_overlapping_bindings(self):
        work_root = self.root / "multi-module-case"
        work_root.mkdir(parents=True)
        module_bytes, _module_stub = build_synthetic_import_prx(b"sceSynthetic", 0)
        module_bytes = bytearray(module_bytes)
        module_phoff = struct.unpack_from("<I", module_bytes, 28)[0]
        struct.pack_into("<I", module_bytes, module_phoff + 28, 0x100)
        main_bytes, _main_stub = build_synthetic_import_prx(b"sceSynthetic", 0x08804000)
        main_bytes = bytearray(main_bytes)
        struct.pack_into("<H", main_bytes, 16, 2)
        struct.pack_into("<I", main_bytes, 24, 0x08804000)
        main_phoff = struct.unpack_from("<I", main_bytes, 28)[0]
        struct.pack_into("<II", main_bytes, main_phoff + 8, 0x08804000, 0x08804000)
        struct.pack_into("<I", main_bytes, main_phoff + 28, 0x100)
        main_shoff = struct.unpack_from("<I", main_bytes, 32)[0]
        main_shentsize, main_shnum = struct.unpack_from("<HH", main_bytes, 46)
        for section_index in range(1, main_shnum - 1):
            address_offset = main_shoff + section_index * main_shentsize + 12
            section_address = struct.unpack_from("<I", main_bytes, address_offset)[0]
            struct.pack_into("<I", main_bytes, address_offset, 0x08804000 + section_address)
        iso_path = work_root / "multi-module.iso"
        create_test_iso_with_modules(
            iso_path,
            bytes(main_bytes),
            sysdir_modules={"alpha.prx": module_bytes},
            usrdir_modules={"beta.elf": module_bytes},
            disc_id="ULUS99998",
            title="Synthetic Multi Module",
        )
        status, report = self._run_module_fixture(iso_path, work_root)

        self.assertEqual(status, 0, report)
        self.assertEqual(report["reached_stage"], "launch")
        self.assertEqual(report["failure_class"], "NONE")
        self.assertTrue(all(stage["status"] == "PASS" for stage in report["stages"].values()))
        self.assertEqual(report["counts"]["modules"], 2)
        self.assertEqual(report["counts"]["encrypted_modules"], 0)
        serialized = json.dumps(report)
        self.assertNotIn("alpha.prx", serialized)
        self.assertNotIn("beta.elf", serialized)
        profile_dir = (
            work_root / "work" / "user-data" / "experimental" / "ULUS99998"
        )
        module_dirs = list(profile_dir.glob("module-stage-*"))
        self.assertEqual(len(module_dirs), 1)
        self.assertEqual(
            sorted(path.name for path in module_dirs[0].iterdir()),
            ["alpha.prx", "beta.elf"],
        )
        profile = json.loads((profile_dir / "profile.json").read_text(encoding="utf-8"))
        modules = profile["manifest"]["modules"]
        self.assertEqual([module["name"] for module in modules], ["alpha.prx", "beta.elf"])
        self.assertTrue(all(module["load_address_evidence"] == "provisional" for module in modules))
        self.assertTrue(all(module["required"] and module["role"] == "guest-prx" for module in modules))
        self.assertNotEqual(modules[0]["load_address"], modules[1]["load_address"])
        self.assertTrue(all(module["load_address"] % 0x10000 == 0 for module in modules))
        nk_cli.validate_bringup_report(report)

    def test_cfw_loader_without_decrypted_original_stops_with_named_finding(self):
        work_root = self.root / "cfw-loader-no-original-case"
        work_root.mkdir(parents=True)
        iso_path = work_root / "cfw-loader.iso"
        patch_module, _stub = build_synthetic_import_prx(b"KUBridge", 0)
        patch_module = bytearray(patch_module)
        phoff = struct.unpack_from("<I", patch_module, 28)[0]
        struct.pack_into("<I", patch_module, phoff + 28, 1)
        create_test_iso_with_modules(
            iso_path,
            build_synthetic_cfw_loader(),
            sysdir_modules={"prometheus.prx": bytes(patch_module)},
            usrdir_modules={},
            old_eboot=build_psp_container(),
            disc_id="ULUS99998",
            title="Synthetic CFW Loader",
        )

        status, report = self._run_module_fixture(iso_path, work_root)

        self.assertEqual(status, 1)
        self.assertEqual(report["reached_stage"], "inspect")
        self.assertEqual(report["failure_class"], "MODIFIED_DUMP_CFW_LOADER")
        self.assertIn(308, report["issue_numbers"])
        self.assertEqual(report["stages"]["prepare_import"]["status"], "NOT_RUN")
        self.assertIn(
            "MODIFIED_DUMP_CFW_LOADER",
            {check["code"] for check in report["preflight_checks"]},
        )
        summary = nk_cli._bringup_human_summary(report)
        self.assertIn("custom-firmware patch", summary)
        self.assertIn("EBOOT.OLD (encrypted)", summary)
        self.assertIn("decrypted/EBOOT.elf", summary)
        self.assertIn("clean dump", summary)
        nk_cli.validate_bringup_report(report)

    def test_cfw_loader_uses_user_original_and_excludes_patch_module(self):
        work_root = self.root / "cfw-loader-original-case"
        work_root.mkdir(parents=True)
        iso_path = work_root / "cfw-loader.iso"
        patch_module, _stub = build_synthetic_import_prx(b"KUBridge", 0)
        patch_module = bytearray(patch_module)
        phoff = struct.unpack_from("<I", patch_module, 28)[0]
        struct.pack_into("<I", patch_module, phoff + 28, 1)
        create_test_iso_with_modules(
            iso_path,
            build_synthetic_cfw_loader(),
            sysdir_modules={"prometheus.prx": bytes(patch_module)},
            usrdir_modules={},
            old_eboot=build_psp_container(),
            disc_id="ULUS99998",
            title="Synthetic CFW Loader",
        )
        decrypted_eboot = build_synthetic_original_elf()

        status, report = self._run_module_fixture(
            iso_path,
            work_root,
            user_decrypted_eboot=decrypted_eboot,
            forbid_iso_executable=True,
        )

        self.assertEqual(status, 0, report)
        self.assertEqual(report["failure_class"], "NONE")
        self.assertEqual(report["reached_stage"], "launch")
        self.assertEqual(report["counts"]["modules"], 0)
        self.assertEqual(report["counts"]["encrypted_modules"], 0)
        self.assertIn(
            "MODIFIED_DUMP_CFW_LOADER",
            {check["code"] for check in report["preflight_checks"]},
        )
        selected_elf = work_root / "work" / "selected.elf"
        self.assertEqual(selected_elf.read_bytes(), decrypted_eboot)
        profile = json.loads(
            (work_root / "work" / "user-data" / "experimental" / "ULUS99998" / "profile.json")
            .read_text(encoding="utf-8")
        )
        self.assertEqual(profile["manifest"]["modules"], [])
        self.assertEqual(
            profile["input_identity"]["selected_executable"].rsplit("/", 1)[-1],
            "EBOOT.BIN",
        )
        nk_cli.validate_bringup_report(report)

    def test_module_placement_without_safe_runtime_range_is_named(self):
        work_root = self.root / "no-module-range-case"
        work_root.mkdir(parents=True)
        iso_path = work_root / "no-module-range.iso"
        create_test_iso_with_modules(
            iso_path,
            build_plain_mips_elf(e_type=2, vaddr=0x09E00000, memsz=0x10000),
            sysdir_modules={
                "module.prx": build_plain_mips_elf(e_type=0xFFA0, vaddr=0, memsz=0x100),
            },
            usrdir_modules={},
            disc_id="ULUS99998",
            title="Synthetic No Module Range",
        )
        status, report = self._run_module_fixture(iso_path, work_root)

        self.assertEqual(status, 1)
        self.assertEqual(report["reached_stage"], "prepare_import")
        self.assertEqual(report["failure_class"], "GUEST_MODULE_LOAD_ADDRESS_LAYOUT_UNAVAILABLE")
        self.assertEqual(report["counts"]["modules"], 1)
        self.assertEqual(report["stages"]["analyze"]["status"], "NOT_RUN")
        self.assertIn(308, report["issue_numbers"])
        nk_cli.validate_bringup_report(report)

    def test_encrypted_iso_module_is_counted_at_crypto_boundary(self):
        work_root = self.root / "encrypted-module-case"
        work_root.mkdir(parents=True)
        iso_path = work_root / "encrypted-module.iso"
        create_test_iso_with_modules(
            iso_path,
            build_plain_mips_elf(e_type=2),
            sysdir_modules={"encrypted.prx": build_psp_container()},
            usrdir_modules={"plain.prx": build_plain_mips_elf(e_type=0xFFA0)},
            disc_id="ULUS99998",
            title="Synthetic Encrypted Module",
        )
        status, report = self._run_module_fixture(iso_path, work_root)
        self.assertEqual(status, 1)
        self.assertEqual(report["reached_stage"], "prepare_import")
        self.assertEqual(report["failure_class"], "GUEST_MODULE_DECRYPTION_REQUIRED")
        self.assertEqual(report["issue_numbers"], [285, 295, 308])
        self.assertEqual(report["counts"]["modules"], 2)
        self.assertEqual(report["counts"]["encrypted_modules"], 1)
        nk_cli.validate_bringup_report(report)

    def test_each_stage_failure_is_named_in_the_report(self):
        expected = {
            "inspect": "INVALID_ISO",
            "prepare_import": "EXPERIMENTAL_IMPORT_FAILED",
            "analyze": "ANALYSIS_FAILED",
            "codegen": "CODEGEN_FAILED",
            "compile": "COMPILE_FAILED",
            "build_package": "BUILD_PACKAGE_FAILED",
            "launch": "UNSUPPORTED_INSTRUCTION",
        }
        for stage, failure_class in expected.items():
            with self.subTest(stage=stage):
                output, report = self._run_case(
                    stage,
                    launch_code=1 if stage == "launch" else 0,
                    launch_output="unsupported instruction" if stage == "launch" else "",
                )
                self.assertNotEqual(output, 0)
                self.assertEqual(report["reached_stage"], stage)
                self.assertEqual(report["failure_class"], failure_class)
                self.assertEqual(report["stages"][stage]["status"], "FAIL")
                nk_cli.validate_bringup_report(report)

    def test_runtime_unimplemented_nid_is_reported_as_unsupported_import(self):
        status, report = self._run_case(
            "launch",
            launch_code=7,
            launch_output=(
                "HLE: unimplemented nid 0x12345678 (unknown) (thread uid 0x1)\n"
                "guest detail at 0x08800000 must not be copied to the report\n"
            ),
        )
        self.assertNotEqual(status, 0)
        self.assertEqual(report["failure_class"], "UNSUPPORTED_IMPORT")
        self.assertEqual(report["runtime_imports"], [{
            "library": "sceSynthetic",
            "nid": "0x12345678",
            "nid_name": None,
        }])
        self.assertEqual(report["runtime_output_kind"], "UNIMPLEMENTED_IMPORT")
        self.assertEqual(report["process_exit_code"], 7)
        self.assertNotIn("08800000", json.dumps(report))
        summary = nk_cli._bringup_human_summary(report)
        self.assertIn("UNSUPPORTED_IMPORT (sceSynthetic, NID 0x12345678)", summary)
        self.assertIn("in the works (", summary)
        self.assertIn("#71", summary)
        nk_cli.validate_bringup_report(report)

    def test_missing_runtime_entry_is_named_without_reporting_address(self):
        status, report = self._run_case(
            "launch",
            launch_code=2,
            launch_output=(
                "no recompiled function at entry 0x08800000 "
                "(no title fallback entry is configured)\n"
            ),
        )
        self.assertNotEqual(status, 0)
        self.assertEqual(report["failure_class"], "ENTRY_NOT_COMPILED")
        self.assertEqual(report["runtime_output_kind"], "ENTRY_NOT_COMPILED")
        self.assertEqual(report["process_exit_code"], 2)
        self.assertNotIn("08800000", json.dumps(report))
        summary = nk_cli._bringup_human_summary(report)
        self.assertIn("in the works (", summary)
        self.assertIn("#296", summary)
        nk_cli.validate_bringup_report(report)

    def test_empty_runtime_exit_reports_process_status(self):
        status, report = self._run_case("launch", launch_code=3)
        self.assertNotEqual(status, 0)
        self.assertEqual(report["runtime_output_kind"], "EMPTY")
        self.assertEqual(report["process_exit_code"], 3)
        self.assertIn("runtime emitted no diagnostic; exit code 3", nk_cli._bringup_human_summary(report))
        nk_cli.validate_bringup_report(report)

    def test_runtime_input_read_failure_hides_the_path(self):
        status, report = self._run_case(
            "launch",
            launch_code=2,
            launch_output="cannot open C:\\synthetic\\private-title.elf\n",
        )
        self.assertNotEqual(status, 0)
        self.assertEqual(report["failure_class"], "RUNTIME_INPUT_UNAVAILABLE")
        self.assertEqual(report["runtime_output_kind"], "DRIVER_INPUT_READ_FAILURE")
        self.assertNotIn("synthetic\\\\private-title", json.dumps(report))
        self.assertIn("in the works (", nk_cli._bringup_human_summary(report))
        nk_cli.validate_bringup_report(report)

    def test_runtime_trace_failure_hides_the_path(self):
        status, report = self._run_case(
            "launch",
            launch_code=2,
            launch_output="no '# init' in C:\\synthetic\\reference.trace\n",
        )
        self.assertNotEqual(status, 0)
        self.assertEqual(report["failure_class"], "RUNTIME_TRACE_UNAVAILABLE")
        self.assertEqual(report["runtime_output_kind"], "DRIVER_TRACE_INPUT_FAILURE")
        self.assertNotIn("reference.trace", json.dumps(report))
        self.assertIn("#297", nk_cli._bringup_human_summary(report))
        nk_cli.validate_bringup_report(report)

    def test_runtime_unimplemented_instruction_hides_address_and_reason(self):
        status, report = self._run_case(
            "launch",
            launch_code=1,
            launch_output=(
                "sr_unimplemented: function 0x08800000: unsupported guest form\n"
                "private retail detail must not be copied to the report\n"
            ),
        )
        self.assertNotEqual(status, 0)
        self.assertEqual(report["failure_class"], "UNSUPPORTED_INSTRUCTION")
        self.assertEqual(report["runtime_output_kind"], "UNSUPPORTED_INSTRUCTION")
        self.assertNotIn("08800000", json.dumps(report))
        self.assertNotIn("private retail detail", json.dumps(report))
        self.assertIn("#118", nk_cli._bringup_human_summary(report))
        nk_cli.validate_bringup_report(report)

    def test_runtime_dispatch_miss_is_named_without_guest_addresses(self):
        status, report = self._run_case(
            "launch",
            launch_code=1,
            launch_output=(
                "DISPATCH_MISS_NEW[1]: target=0x08800000 caller_pc=0x08800004 "
                "ra=0x08800008 uid=0x1\n"
                "--- UNIQUE DISPATCH MISSES SUMMARY (1 entries) ---\n"
                "  target=0x08800000 pc=0x08800004 ra=0x08800008\n"
                "--- END UNIQUE DISPATCH MISSES SUMMARY ---\n"
            ),
        )
        self.assertNotEqual(status, 0)
        self.assertEqual(report["failure_class"], "UNRESOLVED_DISPATCH_TARGET")
        self.assertEqual(report["runtime_output_kind"], "DISPATCH_MISS")
        self.assertIn(118, report["issue_numbers"])
        self.assertNotIn("08800000", json.dumps(report))
        summary = nk_cli._bringup_human_summary(report)
        self.assertIn("unresolved dispatch target", summary)
        self.assertIn("in the works (", summary)
        self.assertIn("#118", summary)
        nk_cli.validate_bringup_report(report)

    def test_launch_timeout_is_a_bounded_reported_exit(self):
        status, report = self._run_case(timeout=True)
        self.assertNotEqual(status, 0)
        self.assertEqual(report["failure_class"], "LAUNCH_TIMEOUT")
        self.assertEqual(report["exit_classification"], "TIMED_OUT")
        self.assertEqual(report["stages"]["launch"]["status"], "FAIL")

    def test_report_schema_rejects_unwhitelisted_and_address_shaped_values(self):
        report = nk_cli._new_bringup_report()
        report["guest_pc"] = "0x08800000"
        with self.assertRaisesRegex(ValueError, "non-whitelisted"):
            nk_cli.validate_bringup_report(report)
        report = nk_cli._new_bringup_report()
        report["unsupported_imports"] = [{"library": "sceKernel", "nid_name": "0x08800000"}]
        with self.assertRaises(ValueError):
            nk_cli.validate_bringup_report(report)


if __name__ == "__main__":
    unittest.main()
