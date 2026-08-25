# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regressions for the source-owned full-production smoke guest."""

from __future__ import annotations

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
GAP_EXPECTED_PRX_SHA256 = "065cfc9092448d5689c922482e1b56d25b2abf56e52568c9582baea7f72f74c4"


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
        # ordinary production dispatcher, targeting the omitted guest address.
        self.assertIn(f"dispatch(s, 0x{generator.HELPER:08x}u);", omitted_text)
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
        build_dir = ROOT / relative_dir
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
            "ge.o", "recomp.o", "guest_interp.o", "title_config.o", "vfpu_tables.o", "debug.o",
            "watchpoints_file.o", "guest_printf.o", "perf.o", "fbcap_policy.o",
            "ge_capture.o", "vfpu_interp.o", "hle.o", "sched.o", "sr_coro.o",
            "iso_unavailable.o", "pgd_unavailable.o", "mpeg.o", "pgf_unavailable.o",
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
        relative_dir = "build/prod-smoke-relabs-check"
        build_dir = self._fabricated_aot_tree(ROOT, relative_dir)
        cwd = os.getcwd()
        try:
            os.chdir(ROOT)
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


if __name__ == "__main__":
    unittest.main()
