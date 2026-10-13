# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Regressions for the source-owned full-production smoke guest."""

from __future__ import annotations

import argparse
import contextlib
from dataclasses import replace
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
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
DISPLAY_GENERATOR_PATH = ROOT / "fixtures" / "display_smoke" / "generate.py"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import analyze  # noqa: E402
import imports as imports_tool  # noqa: E402
import nk_cli  # noqa: E402
import prxload  # noqa: E402
import title_codegen_plan  # noqa: E402
from test_build_truth import _class_scratch_build_root  # noqa: E402
from test_iso_parity import (  # noqa: E402
    build_plain_mips_elf,
    build_psp_container,
    create_test_iso_with_modules,
    create_test_iso_with_executables,
)
from test_import_name_safety import build_synthetic_import_prx  # noqa: E402
from import_fixtures import SYSLIB_EXPORT, build_module_elf  # noqa: E402
from nk_core import package_cache  # noqa: E402
from nk_core.iso_inspect import PBP_BOUNDARY_CODES, runtime_registered_nids  # noqa: E402
from nk_core.types import TitleProfile  # noqa: E402


SPEC = importlib.util.spec_from_file_location("production_smoke_generator", GENERATOR_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load {GENERATOR_PATH}")
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)

DISPLAY_SPEC = importlib.util.spec_from_file_location(
    "display_smoke_generator", DISPLAY_GENERATOR_PATH
)
if DISPLAY_SPEC is None or DISPLAY_SPEC.loader is None:
    raise RuntimeError(f"cannot load {DISPLAY_GENERATOR_PATH}")
display_generator = importlib.util.module_from_spec(DISPLAY_SPEC)
DISPLAY_SPEC.loader.exec_module(display_generator)


EXPECTED_PRX_SHA256 = "bcbc14f27058263bdfbe086f542c22f3d6274539966b521c25c3d90679c546f6"
EXPECTED_PSP_SHA256 = "678288e4c033fb4b3ba6cd9ff0a857daa959aafcc0acfac26e2450e3cc976c5d"
GAP_EXPECTED_PRX_SHA256 = "ebc81e3a82ea47d1745e07d8ae5ffa0133bc74adbfd919db7c64eb1d7305c06f"


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


def build_synthetic_iso_elf() -> bytes:
    """Build a linked ELF that the ISO preflight accepts as EBOOT.BIN."""
    executable, _stub = build_synthetic_import_prx(b"sceSynthetic", 0x08804000)
    executable = bytearray(executable)
    struct.pack_into("<H", executable, 16, 2)
    struct.pack_into("<I", executable, 24, 0x08804000)
    phoff = struct.unpack_from("<I", executable, 28)[0]
    struct.pack_into("<II", executable, phoff + 8, 0x08804000, 0x08804000)
    struct.pack_into("<I", executable, phoff + 28, 0x100)
    shoff = struct.unpack_from("<I", executable, 32)[0]
    shentsize, shnum = struct.unpack_from("<HH", executable, 46)
    for section_index in range(1, shnum - 1):
        address_offset = shoff + section_index * shentsize + 12
        section_address = struct.unpack_from("<I", executable, address_offset)[0]
        struct.pack_into("<I", executable, address_offset, 0x08804000 + section_address)
    return bytes(executable)


def build_synthetic_decrypted_prx() -> bytes:
    """Build a small source-owned ELF module for decrypted-PRX intake tests."""
    module, _stub = build_synthetic_import_prx(b"sceSynthetic", 0)
    module = bytearray(module)
    phoff = struct.unpack_from("<I", module, 28)[0]
    struct.pack_into("<I", module, phoff + 28, 0x100)
    return bytes(module)


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
        self.assertEqual(manifest["relocation_count"], 18)
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
        helper_off = generator.HELPER - generator.BASE
        raw_helper = generator.build_text_segment("aot-gap")[helper_off:helper_off + 0x68]
        self.assertIn(struct.pack("<I", 0x08000026), raw_helper)  # j REGION_B
        self.assertEqual(struct.unpack_from("<I", raw_helper, 0x08)[0], 0x24091234)
        self.assertEqual(struct.unpack_from("<I", raw_helper, 0x0C)[0], 0xAD090000)
        self.assertEqual(struct.unpack_from("<I", raw_helper, 0x10)[0], 0x8D020000)
        self.assertEqual(struct.unpack_from("<I", raw_helper, 0x18)[0], 0x24420001)
        manifest = json.loads((gap_dir / "manifest.json").read_text(encoding="ascii"))
        self.assertEqual(manifest["relocation_count"], 20)

    def test_real_loader_analyzer_and_import_parser_accept_fixture(self):
        loaded = prxload.Prx(self.prx_path, generator.BASE, psp_header=self.psp_path)
        load_segments = [segment for segment in loaded.segments if segment["type"] == 1]
        self.assertEqual(len(load_segments), 2)
        self.assertEqual(loaded.psp_bss_size, 0x40)
        self.assertEqual(loaded.relocate(), 18)
        self.assertEqual(len(loaded.mem), 0x10B8)
        pointer_offset = generator.RESULT_POINTER - generator.BASE
        result_offset = generator.RESULT - generator.BASE
        self.assertEqual(struct.unpack_from("<I", loaded.mem, pointer_offset)[0], generator.RESULT)
        self.assertEqual(struct.unpack_from("<I", loaded.mem, result_offset)[0], 0)
        self.assertEqual(loaded.mem[-0x40:], b"\0" * 0x40)

        elf = analyze.Elf(self.prx_path, base=generator.BASE)
        starts, ranges = analyze.analyze(elf)
        self.assertEqual(
            starts,
            {generator.ENTRY, generator.HELPER, generator.IMPORT_STUB,
             generator.STUB_SET, generator.STUB_GET},
        )
        self.assertEqual(
            ranges,
            [
                (generator.BASE, generator.BASE + generator.TEXT_SECTION_SIZE_AOT),
                (generator.IMPORT_STUB, generator.STUB_GET + 8),
            ],
        )
        self.assertEqual(
            imports_tool.parse_imports(elf),
            {generator.IMPORT_STUB: (generator.LIBRARY, generator.NID),
             generator.STUB_SET: (generator.LIBRARY, generator.NID_SET),
             generator.STUB_GET: (generator.LIBRARY, generator.NID_GET)},
        )

    def test_plain_elf_without_section_names_keeps_imports_as_hle_stubs(self):
        image = bytearray(build_synthetic_original_elf())
        entry = 0x08804000
        stub = entry + 0x80
        struct.pack_into("<I", image, 0x100, 0x0C000000 | ((stub >> 2) & 0x03FFFFFF))
        struct.pack_into("<I", image, 0x104, 0)
        struct.pack_into("<2I", image, 0x108, 0x03E00008, 0)
        elf_path = self.out_dir / "plain_import.elf"
        elf_path.write_bytes(image)

        elf = analyze.Elf(elf_path)
        self.assertIsNone(elf.reloc)
        self.assertIsNone(elf.sec(".sceStub.text"))
        self.assertIsNotNone(elf.sec(".lib.stub"))
        imports = imports_tool.parse_imports(elf)
        self.assertEqual(len(imports), 1)
        stub, (_library, nid) = next(iter(imports.items()))

        output = self.out_dir / "plain_import_recomp.c"
        generated = subprocess.run(
            [sys.executable, str(TOOLS / "codegen.py"), str(elf_path), str(output)],
            cwd=ROOT, capture_output=True, text=True,
        )
        self.assertEqual(generated.returncode, 0, generated.stderr)
        code = output.read_text(encoding="ascii") + "\n".join(
            path.read_text(encoding="ascii")
            for path in sorted(self.out_dir.glob("plain_import_recomp_*.c"))
        )
        self.assertIn(f"void f_{stub:08x}(CpuState *s)", code)
        self.assertIn(f"sr_syscall(s, 0x{nid:08x}u);", code)

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
            f"0x{generator.STUB_GET + 8:08x}u)",
            omitted_text,
        )
        self.assertEqual(omitted_text.count("sr_exec_span_register("), 1)
        self.assertNotIn(f"sr_exec_span_register(0x{generator.DATA_BASE:08x}u", omitted_text)

    def test_result_pointer_relocation_is_load_bearing(self):
        mutated = bytearray(self.prx_path.read_bytes())
        record_index = len(generator.relocation_records()) - 1
        record_offset = generator.RELOCATION_FILE_OFFSET + record_index * 8
        offset, info = struct.unpack_from("<II", mutated, record_offset)
        self.assertEqual(offset, 0x70)
        self.assertEqual(info & 0xF, generator.R_MIPS_32)
        struct.pack_into("<II", mutated, record_offset, offset, info & ~0xF)
        mutated_path = self.out_dir / "guest-no-result-relocation.prx"
        mutated_path.write_bytes(mutated)

        loaded = prxload.Prx(mutated_path, generator.BASE, psp_header=self.psp_path)
        self.assertEqual(loaded.relocate(), 18)
        pointer_offset = generator.RESULT_POINTER - generator.BASE
        self.assertEqual(struct.unpack_from("<I", loaded.mem, pointer_offset)[0],
                         generator.RESULT - generator.DATA_BASE)
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
            f"HLE: calling sceImposeSetLanguageMode (0x{generator.NID_SET:08x})\n"
            f"sceImposeSetLanguageMode: language={generator.IMPOSE_LANG} "
            f"buttonConfirm={generator.IMPOSE_BTN}\n"
            f"HLE: calling sceImposeGetLanguageMode (0x{generator.NID_GET:08x})\n"
            f"DRIVER_EXPECT_U32 addr=0x{generator.SLOT_LANG:08x} "
            f"got=0x{generator.IMPOSE_LANG:08x} "
            f"expected=0x{generator.IMPOSE_LANG:08x} status=PASS\n"
            f"DRIVER_EXPECT_U32 addr=0x{generator.SLOT_BTN:08x} "
            f"got=0x{generator.IMPOSE_BTN:08x} "
            f"expected=0x{generator.IMPOSE_BTN:08x} status=PASS\n"
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
            "no-impose-dispatch": good.replace(
                f"HLE: calling sceImposeSetLanguageMode (0x{generator.NID_SET:08x})\n",
                "",
            ),
            "impose-setter-silent": good.replace(
                f"sceImposeSetLanguageMode: language={generator.IMPOSE_LANG} "
                f"buttonConfirm={generator.IMPOSE_BTN}\n",
                "",
            ),
            "impose-roundtrip-lost": good.replace(
                f"got=0x{generator.IMPOSE_LANG:08x} expected=0x{generator.IMPOSE_LANG:08x} status=PASS",
                f"got=0xDEADBEEF expected=0x{generator.IMPOSE_LANG:08x} status=FAIL",
            ),
            "unimplemented-nid": good + "HLE: unimplemented nid 0x24fd7bcf (sceImposeGetLanguageMode)\n",
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

        image = bytearray(0x10B8)
        struct.pack_into("<I", image, generator.RESULT_POINTER - generator.BASE, generator.RESULT)
        helper_bytes = generator.expected_helper_bytes("aot")
        image[generator.HELPER - generator.BASE:generator.HELPER - generator.BASE + len(helper_bytes)] = helper_bytes
        (build_dir / "production_smoke_image.bin").write_bytes(bytes(image))
        (build_dir / "production_smoke.exe").write_bytes(b"MZ-fake")

        (build_dir / "production_smoke_recomp.c").write_text(
            f"sr_exec_span_register(0x{generator.BASE:08x}u, "
            f"0x{generator.BASE + generator.TEXT_SECTION_SIZE_AOT:08x}u);\n"
            f"sr_exec_span_register(0x{generator.IMPORT_STUB:08x}u, "
            f"0x{generator.STUB_GET + 8:08x}u);\n"
            'fprintf(stderr, "sr_register_all: registered 2 executable span(s)\\n");\n'
            'fprintf(stderr, "sr_register_all: starting 5 registrations\\n");\n'
            "sr_register_chunk_0();\nsr_register_chunk_1();\nsr_register_chunk_2();\n"
            "sr_register_chunk_3();\nsr_register_chunk_4();\n",
            encoding="ascii",
        )
        (build_dir / "production_smoke_recomp_funcs.h").write_text(
            "/* fabricated */\n", encoding="ascii"
        )
        generated = (
            f"void f_{generator.ENTRY:08x}(CpuState *s) {{}}\n"
            f"void f_{generator.HELPER:08x}(CpuState *s) {{}}\n"
            f"void f_{generator.IMPORT_STUB:08x}(CpuState *s) {{}}\n"
            f"void f_{generator.STUB_SET:08x}(CpuState *s) {{}}\n"
            f"void f_{generator.STUB_GET:08x}(CpuState *s) {{}}\n"
            f"sr_syscall(s, 0x{generator.NID:08x}u);\n"
            f"sr_syscall(s, 0x{generator.NID_SET:08x}u);\n"
            f"sr_syscall(s, 0x{generator.NID_GET:08x}u);\n"
        )
        for index in range(5):
            (build_dir / f"production_smoke_recomp_{index}.c").write_text(generated, encoding="ascii")
            (build_dir / f"production_smoke_recomp_{index}.o").write_bytes(b"\0")
        (build_dir / "production_smoke_imports.toml").write_text(
            f'{generator.LIBRARY} = ["0x{generator.NID:08x}", '
            f'"0x{generator.NID_SET:08x}", "0x{generator.NID_GET:08x}"]\n', encoding="ascii"
        )

        required = [
            "production_smoke_recomp.o",
            "production_smoke_recomp_0.o",
            "production_smoke_recomp_1.o",
            "production_smoke_recomp_2.o",
            "production_smoke_recomp_3.o",
            "production_smoke_recomp_4.o",
            "ge.o", "flight_recorder.o", "recomp.o", "guest_interp.o", "title_config.o", "vfpu_tables.o", "debug.o",
            "watchpoints_file.o", "guest_printf.o", "perf.o", "fbcap_policy.o", "fbcap.o",
            "ge_capture.o", "vfpu_interp.o", "hle.o", "sched.o", "sr_coro.o",
            "iso_public.o", "pgd_unavailable.o", "mpeg.o", "pgf_public.o",
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

    def _assert_title_link_uses_response_file(self, makefile):
        """The title link reads its objects from a response file, never the command line.

        A package build under the player's user-data folder expanded the inline
        object list past the Windows command-line limit; the command was cut
        mid-path and ld reported a missing ".../atrac3p" input. Every object the
        compile target depends on must reach the linker through the file."""
        compile_rule = makefile.split("\ncompile: shader-verify ", 1)[1].split("\n\n", 1)[0]
        prerequisites = compile_rule.split("\n", 1)[0].split("|", 1)[0].split()
        recipe = compile_rule.split("\n", 1)[1]
        link_objects = makefile.split("\nLINK_OBJECTS = ", 1)[1].split("\n", 1)[0].split()
        self.assertEqual(sorted(link_objects), sorted(prerequisites))
        self.assertIn("$(file >$(LINK_OBJECTS_RSP),$(subst \\,/,$(LINK_OBJECTS)))", recipe)
        self.assertIn("@$(LINK_OBJECTS_RSP)", recipe)
        for group in link_objects:
            self.assertNotIn(group, recipe, f"{group} is still passed on the link command line")

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
        self._assert_title_link_uses_response_file(makefile)
        gap_recipe =makefile.split("production-smoke-gap:\n", 1)[1].split(
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


class TestStagedRunFailClosed(unittest.TestCase):
    """A broken staging path must fail closed with a named error."""

    STAGING_ERROR = "PRODUCTION_SMOKE_STAGING_FAILED"

    def _dummy_build_dir(self, root: Path) -> Path:
        build_dir = root / "build"
        build_dir.mkdir()
        (build_dir / "production_smoke.exe").write_bytes(b"MZ-stub-not-executed")
        return build_dir

    def test_staging_helper_fails_closed_on_impossible_stage_dir(self):
        with tempfile.TemporaryDirectory(prefix="nk_stage_helper_") as tmp_dir:
            root = Path(tmp_dir)
            build_dir = self._dummy_build_dir(root)
            blocker = root / "not_a_directory"
            blocker.write_text("a file, not a directory", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, self.STAGING_ERROR):
                generator.stage_executable(
                    build_dir / "production_smoke.exe", blocker / "stage"
                )

    def test_run_staged_cli_fails_closed_on_impossible_stage_dir(self):
        with tempfile.TemporaryDirectory(prefix="nk_stage_cli_") as tmp_dir:
            root = Path(tmp_dir)
            build_dir = self._dummy_build_dir(root)
            blocker = root / "not_a_directory"
            blocker.write_text("a file, not a directory", encoding="utf-8")
            result = subprocess.run(
                [
                    sys.executable,
                    str(GENERATOR_PATH),
                    "run-staged",
                    "--build-dir", str(build_dir),
                    "--stage-dir", str(blocker / "stage"),
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(self.STAGING_ERROR, result.stdout + result.stderr)

    def test_run_staged_spawns_the_staged_copy_by_absolute_path(self):
        with tempfile.TemporaryDirectory(prefix="nk_stage_spawn_") as tmp_dir:
            root = Path(tmp_dir)
            build_dir = self._dummy_build_dir(root)
            stage_dir = root / "stage"
            captured = {}

            def fake_run(command, **kwargs):
                captured["command"] = list(command)
                return subprocess.CompletedProcess(list(command), 1, stdout="", stderr="")

            with mock.patch.object(generator.subprocess, "run", side_effect=fake_run):
                with self.assertRaises(RuntimeError):
                    generator.run_staged(build_dir, mode="aot", stage_dir=stage_dir)
            staged = stage_dir / "production_smoke.exe"
            self.assertTrue(staged.is_file())
            self.assertTrue(os.path.isabs(captured["command"][0]))
            self.assertEqual(Path(captured["command"][0]).resolve(), staged.resolve())
            self.assertNotEqual(
                Path(captured["command"][0]).resolve(),
                (build_dir / "production_smoke.exe").resolve(),
            )


class TestProductionSmokePackage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Probes and the packaging route build into one scratch BUILD_ROOT for the class
        # (exported to the package planner's Make runs too), never the checkout's build/.
        cls.binary_dir = _class_scratch_build_root(cls, "production-smoke-package")

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

    def skip_if_sdl3_toolchain_unavailable(self):
        """Prerequisite probe for in-process ``build_package`` calls.

        ``build_package`` streams make's output to the inherited console, so a
        failing build cannot be fed to ``skip_if_toolchain_unusable`` afterwards.
        Probe the Makefile's own ``sdl3-check`` gate first (it has no side
        effects): SDL3 ships with the MSYS2 UCRT64 toolchain (docs/SETUP.md),
        and the gate states the remedy. The documented missing-toolchain
        condition is a SKIP with that remedy; every other build failure stays a
        failure of the route.
        """
        make_name = "mingw32-make" if os.name == "nt" else "make"
        probe = subprocess.run(
            [make_name, "--no-print-directory", "sdl3-check",
             f"BUILD_ROOT={self.binary_dir.as_posix()}"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        self.skip_if_toolchain_unusable(probe)

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
        self.skip_if_sdl3_toolchain_unavailable()
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
        self.skip_if_sdl3_toolchain_unavailable()
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
        self.skip_if_sdl3_toolchain_unavailable()
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
        selected_executable = self.root / "EBOOT.BIN"
        shutil.copyfile(self.fixture_dir / "guest.prx", selected_executable)
        command = self.package_command(executable=selected_executable)
        first = subprocess.run(
            command, cwd=ROOT, env=env, capture_output=True, text=True
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
        self.assertEqual(package["schema_version"], 2)
        self.assertEqual(package["title"]["id"], self.manifest["id"])
        self.assertEqual(package["runtime"]["abi"], "CpuState")
        # The package's ABI is the one the runtime header defines, not a value read back from
        # the generator that wrote it (RuntimeAbiPinTests pins the flight schema the same way).
        header = (ROOT / "src" / "rt" / "recomp.h").read_text(encoding="utf-8")
        match = re.search(r"^#define SR_CPUSTATE_ABI_VERSION (\d+)u$", header, re.MULTILINE)
        self.assertIsNotNone(match, "recomp.h must define SR_CPUSTATE_ABI_VERSION <n>u")
        self.assertEqual(package["runtime"]["abi_version"], int(match.group(1)))
        self.assertEqual(package["runtime"]["abi_version"], title_codegen_plan.codegen_abi_version())
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
            command, cwd=ROOT, env=env, capture_output=True, text=True
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
        package_cache.write_local_title_input_identity(
            user_root, package["title_input_identity"]
        )
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
            ["mingw32-make", "--no-print-directory", "player-state-test-bin",
             f"BUILD_ROOT={self.binary_dir.as_posix()}"],
            cwd=ROOT, env=player_env, capture_output=True, text=True,
        )
        self.assertEqual(native_build.returncode, 0, native_build.stdout + native_build.stderr)
        validator = self.binary_dir / "test_player_state.exe"
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
        self.assertNotRegex(result.stderr, r"#[0-9]+")
        self.assertIn("matching local key file", result.stderr)
        self.assertNotIn("automatic decryption is in the works", result.stderr.lower())
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
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertNotIn("production PGF/PGD runtime backends", completed.stderr)
        self.assertTrue((package_dir / "package.json").is_file())
        self.assertTrue((package_dir / "build-report.json").is_file())
        report = json.loads((package_dir / "build-report.json").read_text(encoding="utf-8"))
        self.assertEqual(report.get("backends"), "public")
        self.assertIn("fonts: import your own PSP fonts; the public PGF reader is available for supported inputs (#474)", report.get("limits", []))
        self.assertIn("PGD-protected data: unavailable; broader ISO-to-Play support is in the works (#308)", report.get("limits", []))

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


class TestCodegenStageReuse(unittest.TestCase):
    """Bring-up generates each title's code once: the package compiles the codegen
    stage's output, but only after the checks the native-only reuse of a package's
    generated C performs, and any refusal regenerates under a named reason."""

    @classmethod
    def setUpClass(cls):
        # The borrowed skip helper builds its probe under the class scratch BUILD_ROOT,
        # the same way TestProductionSmokePackage does, never the checkout's build/.
        cls.binary_dir = _class_scratch_build_root(cls, "codegen-stage-reuse")

    setUp = TestProductionSmokePackage.setUp
    skip_if_toolchain_unusable = TestProductionSmokePackage.skip_if_toolchain_unusable
    skip_if_sdl3_toolchain_unavailable = (
        TestProductionSmokePackage.skip_if_sdl3_toolchain_unavailable
    )
    GAME = "production_smoke"

    def require_make(self):
        required = ("mingw32-make", "gcc", "pwsh") if os.name == "nt" else ("make", "gcc")
        if not all(shutil.which(name) for name in required):
            self.skipTest("the package route requires " + ", ".join(required))

    def build(self, output_dir: Path, sink=None, **options):
        stdout = sink if sink is not None else io.StringIO()
        with contextlib.redirect_stdout(stdout):
            result = title_codegen_plan.build_package(
                self.manifest,
                manifest_path=self.manifest_path,
                game_elf=self.fixture_dir / "guest.prx",
                psp_header=self.fixture_dir / "guest.psp",
                output_dir=output_dir,
                funcs_per_chunk=1,
                public_safe=True,
                **options,
            )
        return result, stdout.getvalue()

    def make_stage(self, name: str = "codegen-stage") -> Path:
        stage = self.root / name
        result, _output = self.build(stage, aot_stage=True)
        self.assertEqual(result["format"], "nakagawa-aot-stage")
        self.assertTrue((stage / package_cache.COMPLETION_MANIFEST).is_file())
        return stage

    def generated_code(self, directory: Path) -> dict[str, bytes]:
        if not directory.is_dir():
            return {}
        return {
            path.name: path.read_bytes()
            for path in directory.iterdir()
            if path.is_file() and title_codegen_plan._is_aot_output(path.name, self.GAME)
        }

    def attempt(self, stage: Path, output_dir: Path, *, environment=None):
        """Run the package route against ``stage`` with its Make call recorded, not run.

        Returns the Make target and NK_AOT_PREGENERATED the route chose, and the
        route's printed output. Make never runs, so the package is incomplete.
        """
        real_run = subprocess.run
        calls = []
        sink = io.StringIO()

        def record_make(command, *args, **kwargs):
            if "--no-print-directory" in command and command[-1] in {"all", "compile"}:
                calls.append((command[-1], kwargs["env"]["NK_AOT_PREGENERATED"]))
                return subprocess.CompletedProcess(command, 0)
            return real_run(command, *args, **kwargs)

        with (
            mock.patch.dict(os.environ, environment or {}),
            mock.patch.object(title_codegen_plan.subprocess, "run", side_effect=record_make),
            self.assertRaises(title_codegen_plan.PackageRouteError) as caught,
        ):
            self.build(output_dir, sink=sink, reuse_aot_stage=stage)
        self.assertEqual(caught.exception.code, "PACKAGE_BUILD_INCOMPLETE")
        self.assertEqual(len(calls), 1, calls)
        return calls[0], sink.getvalue()

    def assert_refused(self, stage: Path, code: str, *, environment=None, name: str):
        output_dir = self.root / name
        make_call, output = self.attempt(stage, output_dir, environment=environment)
        lines = [line for line in output.splitlines() if line.startswith("AOT_STAGE_REUSE:")]
        self.assertEqual(len(lines), 1, output)
        self.assertTrue(lines[0].startswith(f"AOT_STAGE_REUSE: REFUSED {code}"), lines[0])
        # A refused stage is regenerated from source: the whole build, nothing copied.
        self.assertEqual(make_call, ("all", "0"))
        self.assertEqual(self.generated_code(output_dir), {})
        return lines[0]

    def test_stage_needs_an_empty_dedicated_directory(self):
        stage = self.root / "occupied-stage"
        stage.mkdir()
        (stage / f"{self.GAME}_recomp_9.c").write_bytes(b"left over from an earlier run")
        with self.assertRaises(title_codegen_plan.PackageRouteError) as caught:
            self.build(stage, aot_stage=True)
        self.assertEqual(caught.exception.code, "PACKAGE_OUTPUT_CONFLICT")
        with self.assertRaises(title_codegen_plan.PackageRouteError) as caught:
            self.build(self.root / "both", aot_stage=True, reuse_aot_stage=stage)
        self.assertEqual(caught.exception.code, "PACKAGE_REUSE_INVALID")

    def test_accepted_stage_is_compiled_without_generating_again(self):
        self.require_make()
        stage = self.make_stage()
        output_dir = self.root / "accepted"
        make_call, output = self.attempt(stage, output_dir)
        self.assertIn("AOT_STAGE_REUSE: ACCEPTED AOT_STAGE_ACCEPTED", output)
        self.assertEqual(make_call, ("compile", "1"))
        copied = self.generated_code(output_dir)
        self.assertEqual(copied, self.generated_code(stage))
        self.assertTrue(set(title_codegen_plan._aot_required_outputs(self.GAME)) <= set(copied))

    def test_every_mismatch_is_refused_by_name_and_regenerated(self):
        self.require_make()
        # A tampered stage output.
        tampered = self.make_stage("tampered-stage")
        chunk = sorted(tampered.glob(f"{self.GAME}_recomp_[0-9]*.c"))[0]
        chunk.write_bytes(chunk.read_bytes() + b"\n/* edited */\n")
        self.assert_refused(tampered, "AOT_STAGE_ARTIFACT_MISMATCH", name="tampered")

        # A missing completion record (an interrupted stage).
        unrecorded = self.make_stage("unrecorded-stage")
        (unrecorded / package_cache.COMPLETION_MANIFEST).unlink()
        self.assert_refused(unrecorded, "AOT_STAGE_RECORD_MISSING", name="unrecorded")

        stage = self.make_stage()
        # A changed input: the package is built from a manifest whose bytes differ.
        original_manifest = self.manifest_path.read_bytes()
        self.manifest_path.write_bytes(original_manifest + b"\n")
        try:
            line = self.assert_refused(stage, "AOT_STAGE_INPUT_CHANGED", name="input")
        finally:
            self.manifest_path.write_bytes(original_manifest)
        self.assertIn("aot:manifest_sha256", line)

        # Changed codegen flags.
        line = self.assert_refused(
            stage, "AOT_STAGE_OPTIONS_CHANGED",
            environment={"CODEGEN_USER_ARGS": "--nan-trap"}, name="flags",
        )
        self.assertIn("aot:codegen_options_sha256", line)

        # A changed code generator.
        real_sha256_file = package_cache.sha256_file

        def changed_generator(path):
            if Path(path).name == "codegen.py":
                return "c" * 64
            return real_sha256_file(path)

        with mock.patch.object(package_cache, "sha256_file", side_effect=changed_generator):
            line = self.assert_refused(stage, "AOT_STAGE_GENERATOR_CHANGED", name="generator")
        self.assertIn("aot:codegen_sha256", line)

        # The stage itself was never modified by any refusal and is still accepted.
        make_call, _output = self.attempt(stage, self.root / "still-accepted")
        self.assertEqual(make_call, ("compile", "1"))

    def test_stage_changed_after_its_check_is_refused_and_leaves_nothing(self):
        self.require_make()
        stage = self.make_stage()
        real_evaluate = package_cache.evaluate_aot_stage

        def evaluate_then_edit(*args, **kwargs):
            decision = real_evaluate(*args, **kwargs)
            chunk = sorted(stage.glob(f"{self.GAME}_recomp_[0-9]*.c"))[-1]
            chunk.write_bytes(chunk.read_bytes() + b"\n/* edited after the check */\n")
            return decision

        destination = self.root / "raced"
        key = package_cache.read_bounded_json(
            stage / package_cache.COMPLETION_MANIFEST,
            max_bytes=package_cache.MAX_CACHE_JSON_BYTES,
            max_depth=package_cache.MAX_CACHE_JSON_DEPTH,
            max_members=package_cache.MAX_CACHE_JSON_MEMBERS,
            max_items=package_cache.MAX_CACHE_JSON_ITEMS,
            max_nodes=package_cache.MAX_CACHE_JSON_NODES,
        )["cache_key"]
        with mock.patch.object(package_cache, "evaluate_aot_stage", side_effect=evaluate_then_edit):
            decision = title_codegen_plan._accept_aot_stage(stage, key, destination, self.GAME)
        self.assertFalse(decision.accepted)
        self.assertEqual(decision.code, package_cache.AOT_STAGE_ARTIFACT_MISMATCH)
        self.assertIn("completion artifact digest mismatch", decision.detail)
        self.assertEqual(self.generated_code(destination), {})

    def test_compiled_stage_packages_the_bytes_regeneration_packages(self):
        """Failing-before: generation ran twice for one package (stage, then Make).

        The package built from an accepted stage holds the same generated C,
        objects and executable as the package built by generating again, at the
        same path, from the same inputs.
        """
        self.require_make()
        self.skip_if_sdl3_toolchain_unavailable()
        stage = self.make_stage()
        output_dir = self.root / "pkg"

        def built_bytes(directory: Path) -> dict[str, bytes]:
            return {
                path.relative_to(directory).as_posix(): path.read_bytes()
                for path in sorted(directory.rglob("*"))
                if path.is_file() and (
                    title_codegen_plan._is_aot_output(path.name, self.GAME)
                    or path.suffix in {".o", ".exe"}
                    or path.name in {"package.json", "build-report.json", self.GAME}
                )
            }

        _package, regenerated_output = self.build(output_dir)
        self.assertNotIn("AOT_STAGE_REUSE:", regenerated_output)
        regenerated = built_bytes(output_dir)
        shutil.move(output_dir, self.root / "regenerated")

        _package, reused_output = self.build(output_dir, reuse_aot_stage=stage)
        self.assertIn("AOT_STAGE_REUSE: ACCEPTED AOT_STAGE_ACCEPTED", reused_output)
        reused = built_bytes(output_dir)
        self.assertEqual(sorted(reused), sorted(regenerated))
        for name, data in regenerated.items():
            self.assertEqual(reused[name], data, name)
        self.assertTrue(any(name.endswith(".o") for name in reused))
        valid, reason = package_cache.validate_package_cache(output_dir)
        self.assertTrue(valid, reason)


class TestFlightSmokeProjection(unittest.TestCase):
    @staticmethod
    def _bundle(set_fb_args):
        events = [{"class": "ge", "kind": 20, "sequence": 1,
                   "arg0": 1, "arg1": 0, "arg2": 0, "arg3": 0}]
        for args in set_fb_args:
            events.append({"class": "present", "kind": 28, "sequence": len(events) + 1,
                           "arg0": args[0], "arg1": 3, "arg2": 512, "arg3": args[1]})
            events.append({"class": "present", "kind": 29, "sequence": len(events) + 1,
                           "arg0": 0, "arg1": 0, "arg2": 0, "arg3": 0})
        return {"events": events, "recorder": {"recorded": len(events), "dropped": 0},
                "terminal": {"sequence": len(events)}}

    def test_set_framebuf_vblank_count_is_not_compared(self):
        """The VBLANK count at SetFrameBuf follows host pacing; two identical runs differ in it."""
        first = display_generator._without_host_present_events(
            self._bundle([(0x04000000, 32), (0x04088000, 33)]))
        second = display_generator._without_host_present_events(
            self._bundle([(0x04000000, 28), (0x04088000, 30)]))
        self.assertEqual(first, second)
        self.assertEqual([e["kind"] for e in first["events"]], [20, 28, 28])

    def test_set_framebuf_buffer_address_is_still_compared(self):
        first = display_generator._without_host_present_events(
            self._bundle([(0x04000000, 32)]))
        second = display_generator._without_host_present_events(
            self._bundle([(0x04088000, 32)]))
        self.assertNotEqual(first, second)


    def test_projection_leaves_ge_event_arguments_untouched(self):
        """The draw-count mutation check depends on arg3 surviving outside SetFrameBuf."""
        bundle = self._bundle([(0x04000000, 32)])
        bundle["events"][0].update({"kind": 25, "arg3": 7})
        projected = display_generator._without_host_present_events(bundle)
        self.assertEqual(projected["events"][0]["arg3"], 7)

    def test_a_stalled_vblank_count_is_refused(self):
        display_generator._require_vblank_progress(
            self._bundle([(0x04000000, 30), (0x04088000, 30), (0x04000000, 31)]), "healthy")
        for counts in ((30, 30, 30), (31, 30, 32)):
            with self.subTest(counts=counts):
                bundle = self._bundle([(0x04000000, count) for count in counts])
                with self.assertRaises(RuntimeError):
                    display_generator._require_vblank_progress(bundle, "stalled")


    def test_a_single_frame_run_is_not_blamed_on_the_vblank_clock(self):
        display_generator._require_vblank_progress(self._bundle([(0x04000000, 5)]), "one-frame")

    def test_flight_smoke_checks_vblank_progress_before_projecting(self):
        """Both runs are gated before the host-paced count is zeroed for the comparison."""
        source = DISPLAY_GENERATOR_PATH.read_text(encoding="utf-8")
        body = source[source.index("def flight_smoke("):source.index("def run_player(")]
        loop = body.index('for label, bundle in (("first", first), ("second", second)):')
        gate = body.index("_require_vblank_progress(bundle, label)")
        self.assertLess(loop, gate)
        # The gate is inside the loop over both runs: nothing but loop body lies between.
        self.assertNotIn("\n        kind_counts", body[loop:gate])
        self.assertLess(gate, body.index("_without_host_present_events(source)"))


class TestPresenterContract(unittest.TestCase):
    def test_gdi_acceptance_requires_window_dc_and_positive_scanlines(self):
        """The headless suite cannot open GDI, so pin its acceptance contract in source."""
        source = (ROOT / "src" / "rt" / "gui.c").read_text(encoding="utf-8")
        presenter = source[source.index("int gui_present"):]
        accepted = presenter[:presenter.index("#else")]
        # The present accepts a GDI frame only when the blit drew it; a failed blit
        # resolves an armed capture as failed and reports the frame as not presented.
        self.assertRegex(
            accepted,
            r"if \(!gdi_blit\(\)\)\s*\{\s*sr_capture_fail\([^;]*\);\s*return 0;\s*\}",
        )
        gdi = source[source.index("static int gdi_blit(void)"):source.index("int gui_present")]
        self.assertIn("if (!s_hwnd) return 0;", gdi)
        self.assertIn("HDC dc = GetDC(s_hwnd);", gdi)
        self.assertIn("if (!dc) return 0;", gdi)
        self.assertRegex(
            gdi,
            r"if \(!GetClientRect\(s_hwnd, &cr\)\)\s*\{\s*"
            r"ReleaseDC\(s_hwnd, dc\);\s*return 0;\s*\}",
        )
        self.assertLess(
            gdi.index("if (!GetClientRect(s_hwnd, &cr))"),
            gdi.index("int scanlines = StretchDIBits"),
        )
        self.assertIn("int scanlines = StretchDIBits", gdi)
        self.assertIn("return scanlines > 0;", gdi)

    def test_offscreen_frame_event_is_quiet_without_present_trace(self):
        source = (ROOT / "src" / "rt" / "gui.c").read_text(encoding="utf-8")
        presenter = source[source.index("int gui_present"):]
        offscreen = presenter[presenter.index("if (s_offscreen)"):presenter.index("int accepted")]
        self.assertRegex(
            offscreen,
            r'if \(getenv\("SR_PRESENT_TRACE"\)\)\s*\{\s*'
            r'sr_gui_boot_event\("BOOT_EVENT phase=frame_present backend=offscreen',
        )

    def test_display_smoke_offscreen_pins_inherited_sdl_selectors(self):
        inherited = {
            "SDL_VIDEO_DRIVER": "windows",
            "SDL_VIDEODRIVER": "windows",
            "SDL_AUDIO_DRIVER": "wasapi",
            "SDL_AUDIODRIVER": "wasapi",
        }
        expected = f"0x{display_generator.final_frame_first_pixel(1):08x}"
        combined = (
            "BOOT_EVENT phase=init public_safe=1\n"
            "BOOT_EVENT phase=image_loaded entry=0x08810000\n"
            "BOOT_EVENT phase=runtime_registered entry=0x08810000\n"
            f"DRIVER_EXPECT_U32 addr=0x04000000 got={expected} "
            f"expected={expected} status=PASS\n"
            "BOOT_EVENT phase=frame_present backend=offscreen frame=1\n"
            "HOST_PRESENT_SUBMITTED f=2 buf=0x04000000 fmt=3 stride=512\n"
        )
        captured = {}

        def fake_run(command, **kwargs):
            captured["env"] = dict(kwargs["env"])
            return subprocess.CompletedProcess(command, 0, combined, "")

        with tempfile.TemporaryDirectory(prefix="display-smoke-env-") as temp_dir:
            build_dir = Path(temp_dir)
            fixture = build_dir / "fixture"
            fixture.mkdir()
            (fixture / "manifest.json").write_text('{"frames": 1}\n', encoding="ascii")
            suffix = ".exe" if os.name == "nt" else ""
            (build_dir / f"{display_generator.ARTIFACT_STEM}{suffix}").write_bytes(b"runtime")
            (build_dir / f"{display_generator.ARTIFACT_STEM}_image.bin").write_bytes(b"image")
            with mock.patch.dict(os.environ, inherited):
                with mock.patch.object(
                    display_generator.subprocess, "run", side_effect=fake_run
                ):
                    status = display_generator.run(build_dir, gui=True, offscreen=True)

        self.assertEqual(status, 0)
        for selector in inherited:
            self.assertEqual(captured["env"][selector], "dummy")
        self.assertEqual(captured["env"]["SR_VIDEO"], "offscreen")
        self.assertEqual(captured["env"]["SR_PRESENT_TRACE"], "1")

    def test_display_smoke_routes_keep_separate_logs(self):
        expected = f"0x{display_generator.final_frame_first_pixel(1):08x}"
        combined = (
            "BOOT_EVENT phase=init public_safe=1\n"
            "BOOT_EVENT phase=image_loaded entry=0x08810000\n"
            "BOOT_EVENT phase=runtime_registered entry=0x08810000\n"
            f"DRIVER_EXPECT_U32 addr=0x04000000 got={expected} "
            f"expected={expected} status=PASS\n"
            "BOOT_EVENT phase=frame_present backend=offscreen frame=1\n"
            "HOST_PRESENT_SUBMITTED f=2 buf=0x04000000 fmt=3 stride=512\n"
        )

        def fake_run(command, **kwargs):
            route = "offscreen" if kwargs["env"].get("SR_VIDEO") == "offscreen" else "headless"
            return subprocess.CompletedProcess(command, 0, f"{route}\n{combined}", "")

        with tempfile.TemporaryDirectory(prefix="display-smoke-logs-") as temp_dir:
            build_dir = Path(temp_dir)
            fixture = build_dir / "fixture"
            fixture.mkdir()
            (fixture / "manifest.json").write_text('{"frames": 1}\n', encoding="ascii")
            suffix = ".exe" if os.name == "nt" else ""
            (build_dir / f"{display_generator.ARTIFACT_STEM}{suffix}").write_bytes(b"runtime")
            (build_dir / f"{display_generator.ARTIFACT_STEM}_image.bin").write_bytes(b"image")
            with mock.patch.object(display_generator.subprocess, "run", side_effect=fake_run):
                self.assertEqual(display_generator.run(build_dir), 0)
                self.assertEqual(display_generator.run(build_dir, gui=True, offscreen=True), 0)
            stem = display_generator.ARTIFACT_STEM
            headless = (build_dir / f"{stem}.stdout.log").read_text(encoding="utf-8")
            offscreen = (build_dir / f"{stem}.gui-offscreen.stdout.log").read_text(encoding="utf-8")
        self.assertTrue(headless.startswith("headless\n"))
        self.assertTrue(offscreen.startswith("offscreen\n"))

    def test_display_smoke_offscreen_without_gui_is_a_usage_error(self):
        with self.assertRaises(SystemExit) as raised, contextlib.redirect_stderr(None):
            display_generator.parse_args(["run", "--build-dir", "x", "--offscreen"])
        self.assertEqual(raised.exception.code, 2)

    def test_offscreen_present_evidence_rejects_malformed_submission_marker(self):
        combined = (
            "BOOT_EVENT phase=frame_present backend=offscreen frame=1\n"
            "HOST_PRESENT_SUBMITTED nonsense\n"
        )
        failure = display_generator.offscreen_present_evidence_failure(combined)
        self.assertIsNotNone(failure)
        self.assertIn("malformed HOST_PRESENT_SUBMITTED marker", failure)

    def test_offscreen_present_evidence_rejects_malformed_frame_marker(self):
        combined = (
            "BOOT_EVENT phase=frame_present backend=offscreen BROKEN\n"
            "HOST_PRESENT_SUBMITTED f=2 buf=0x04000000 fmt=3 stride=512\n"
        )
        failure = display_generator.offscreen_present_evidence_failure(combined)
        self.assertIsNotNone(failure)
        self.assertIn("malformed offscreen frame_present marker", failure)


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

    def _run_case(
        self, failure=None, *, launch_code=0, launch_output="", timeout=False,
        flight_events=None, flight_dropped=0, instruction_trace=False,
        unsupported_opcodes=None, analyzer_error=None,
    ):
        case_root = self.root / (failure or "success")
        report_path = case_root / "bringup.json"
        args = argparse.Namespace(
            iso=str(self.iso),
            work_dir=str(case_root / "work"),
            report=str(report_path),
            launch_timeout=1,
            instruction_trace=instruction_trace,
            private_sweep_import_report=str(case_root / "work" / "sweep-imports.json"),
        )
        globals_timeout = timeout
        launch_commands = []
        launch_envs = []

        class FakeProcess:
            def __init__(self, output):
                self.returncode = launch_code
                self.output = output

            def communicate(self, timeout=None):
                if timeout is not None and globals_timeout:
                    raise subprocess.TimeoutExpired("synthetic-runtime", timeout)
                return self.output, None

            def kill(self):
                self.returncode = -9

        def fake_package_build(build_args, stage_observer=None):
            self.last_build_arguments = build_args
            # The package build runs the codegen stage first (into build_args.aot_stage_dir).
            stage_observer("codegen", "START", 0)
            if failure == "codegen":
                stage_observer("codegen", "FAIL", 3)
                build_args.aot_stage_result = {"status": "NOT_RUN", "reason": "NONE"}
                return 1
            stage_observer("codegen", "PASS", 3)
            build_args.aot_stage_result = {"status": "REUSED", "reason": "NONE"}
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

        def fake_popen(command, **kwargs):
            launch_commands.append(list(command))
            launch_envs.append(dict(kwargs.get("env", {})))
            flight_path = kwargs.get("env", {}).get("SR_FLIGHT_OUTPUT")
            if flight_path:
                events = flight_events
                if events is None:
                    events = [{"class": "hle", "kind": 1, "arg0": 0x289D82FE}]
                Path(flight_path).write_text(json.dumps({
                    "recorder": {"dropped": flight_dropped},
                    "events": events,
                }), encoding="utf-8")
            output = launch_output
            if not output and launch_code == 0:
                output = (
                    "BOOT_EVENT phase=window_ready backend=offscreen\n"
                    "BOOT_EVENT phase=frame_present backend=offscreen frame=1\n"
                    "HOST_PRESENT_SUBMITTED f=1 buf=0x04000000 fmt=3 stride=512\n"
                )
            return FakeProcess(output)

        completed = subprocess.CompletedProcess(["synthetic-codegen"], 1 if failure == "codegen" else 0, "", "")
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(nk_cli.subprocess, "run", return_value=completed))
            stack.enter_context(mock.patch.object(
                nk_cli.subprocess, "Popen", side_effect=fake_popen
            ))
            stack.enter_context(mock.patch.object(
                nk_cli, "cmd_build_package", side_effect=fake_package_build
            ))
            if unsupported_opcodes is not None:
                stack.enter_context(mock.patch.object(
                    nk_cli, "_count_unsupported_opcodes", return_value=unsupported_opcodes
                ))
            if failure == "prepare_import":
                stack.enter_context(mock.patch.object(
                    nk_cli, "write_experimental_profile", side_effect=RuntimeError("synthetic refusal")
                ))
            if failure == "analyze":
                stack.enter_context(mock.patch.object(
                    title_codegen_plan, "_make_input_images",
                    side_effect=analyzer_error or RuntimeError("synthetic refusal"),
                ))
            if failure == "inspect":
                broken_iso = case_root / "broken.iso"
                broken_iso.parent.mkdir(parents=True, exist_ok=True)
                broken_iso.write_bytes(b"invalid synthetic image")
                args.iso = str(broken_iso)
            with mock.patch("builtins.print"):
                status = nk_cli.cmd_bringup(args)
        self.last_launch_command = launch_commands[-1] if launch_commands else None
        self.last_launch_env = launch_envs[-1] if launch_envs else None
        return status, json.loads(report_path.read_text(encoding="utf-8"))

    def test_unsupported_opcodes_describe_the_packaged_stub_report(self):
        """The report counts the stub report that shipped, not a separate codegen run."""
        counted = []

        def count(path, _sources):
            counted.append(Path(path))
            return {"SPECIAL": 1}

        with mock.patch.object(nk_cli, "_count_unsupported_opcodes", side_effect=count):
            status, report = self._run_case()
        self.assertEqual(status, 0, report)
        build_args = self.last_build_arguments
        package_dir = build_args.user_data_root / "packages" / "ULUS99998"
        self.assertEqual(counted, [package_dir / "runtime_recomp_stubs.txt"])
        self.assertEqual(report["counts"]["unsupported_opcodes"], {"SPECIAL": 1})
        # The codegen stage is handed to the package build, which reports its decision.
        self.assertEqual(build_args.aot_stage_dir.name, "codegen-stage")
        self.assertEqual(report["codegen_reuse"], {"status": "REUSED", "reason": "NONE"})
        nk_cli.validate_bringup_report(report)

        counted.clear()
        with mock.patch.object(nk_cli, "_count_unsupported_opcodes", side_effect=count):
            status, report = self._run_case("compile")
        self.assertEqual(status, 1, report)
        self.assertEqual(report["failure_class"], "COMPILE_FAILED")
        # Nothing was packaged, so the generated stage output is what is described.
        self.assertEqual(len(counted), 1)
        self.assertEqual(counted[0].parent, self.last_build_arguments.aot_stage_dir)
        self.assertTrue(counted[0].name.endswith("_recomp_stubs.txt"))
        nk_cli.validate_bringup_report(report)

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
        self.assertEqual(command[5:], ["none", "none", "--sched", "--gui"])
        self.assertEqual(self.last_launch_env.get("SR_PRESENT_TRACE"), "1")
        for selector in (
            "SDL_VIDEO_DRIVER",
            "SDL_VIDEODRIVER",
            "SDL_AUDIO_DRIVER",
            "SDL_AUDIODRIVER",
        ):
            self.assertEqual(self.last_launch_env.get(selector), "dummy")
        self.assertEqual(self.last_launch_env.get("SR_VIDEO"), "offscreen")
        self.assertNotIn("SR_GEWATCH", self.last_launch_env)
        self.assertEqual(report["presentation"], {
            "status": "FRAME_SUBMITTED",
            "frame_submissions": 1,
            "backend": "offscreen",
        })
        summary = nk_cli._bringup_human_summary(report)
        self.assertIn("offscreen presenter accepted 1 frame submission(s)", summary)
        self.assertIn("Visual contents are not verified", summary)
        self.assertFalse(self.last_build_arguments.instruction_trace)
        nk_cli.validate_bringup_report(report)

    def test_invalid_selected_boot_sources_write_schema_valid_unsupported_report(self):
        invalid_sources = (".", "..", "BAD/NAME", "BAD\x01NAME", "A" * 256)
        for index, source in enumerate(invalid_sources):
            with self.subTest(source=repr(source)):
                case_root = self.root / f"invalid-boot-source-{index}"
                report_path = case_root / "bringup.json"
                args = argparse.Namespace(
                    iso=str(self.iso),
                    work_dir=str(case_root / "work"),
                    report=str(report_path),
                    launch_timeout=1,
                    instruction_trace=False,
                )
                preflight = {
                    "checks": [
                        {"code": "DISC_SFO", "status": "OK", "issues": []},
                        {"code": "EXECUTABLE", "status": "OK", "issues": []},
                    ],
                    "selected_executable": "EBOOT.BIN",
                    "selected_executable_source": source,
                    "modified_dump_cfw_loader": False,
                }
                with (
                    mock.patch.object(
                        nk_cli, "inspect_compatibility_preflight", return_value=preflight
                    ),
                    mock.patch("builtins.print"),
                ):
                    status = nk_cli.cmd_bringup(args)

                self.assertEqual(status, 1)
                report = json.loads(report_path.read_text(encoding="utf-8"))
                nk_cli.validate_bringup_report(report)
                self.assertEqual(report["reached_stage"], "inspect")
                self.assertEqual(report["stages"]["inspect"]["status"], "FAIL")
                self.assertEqual(report["failure_class"], "EXECUTABLE_UNSUPPORTED")
                self.assertEqual(report["issue_numbers"], [308])

    def test_pbp_package_keeps_its_named_boundary_in_the_report(self):
        from test_iso_parity import build_pbp_package

        case_root = self.root / "pbp-package"
        package = case_root / "store-package.iso"
        case_root.mkdir(parents=True)
        build_pbp_package(package, disc_id="ULUS99997", title="Store Package")
        report_path = case_root / "bringup.json"
        args = argparse.Namespace(
            iso=str(package),
            work_dir=str(case_root / "work"),
            report=str(report_path),
            launch_timeout=1,
            instruction_trace=False,
        )
        with mock.patch("builtins.print"):
            status = nk_cli.cmd_bringup(args)

        self.assertEqual(status, 1)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        nk_cli.validate_bringup_report(report)
        self.assertEqual(report["reached_stage"], "inspect")
        self.assertEqual(report["stages"]["inspect"]["status"], "FAIL")
        self.assertEqual(report["failure_class"], "PBP_PACKAGE_UNSUPPORTED")

    def test_every_identify_boundary_code_is_a_schema_failure_class(self):
        # The sweep and the bring-up report only carry failure classes the schema
        # enumerates; an unlisted identify boundary is silently replaced by
        # INVALID_ISO, which hides the named refusal from every downstream reader.
        schema = json.loads(nk_cli.BRINGUP_SCHEMA_PATH.read_text(encoding="utf-8"))
        enumerated = set(schema["properties"]["failure_class"]["enum"])
        # The expected set is the emitter's own registry, never a copy of it here.
        self.assertTrue(PBP_BOUNDARY_CODES)
        self.assertEqual(set(PBP_BOUNDARY_CODES) - enumerated, set())
        for code in PBP_BOUNDARY_CODES:
            with self.subTest(code=code):
                report = nk_cli._new_bringup_report()
                report["reached_stage"] = "inspect"
                report["stages"]["inspect"] = {"status": "FAIL", "duration_ms": 0}
                report["failure_class"] = code
                nk_cli.validate_bringup_report(report)

    def test_sanitized_report_preserves_offscreen_backend_with_perf_timestamp(self):
        output = (
            "BOOT_EVENT phase=window_ready backend=offscreen t_ns=100\n"
            "BOOT_EVENT phase=frame_present backend=offscreen frame=1 t_ns=101\n"
            "HOST_PRESENT_SUBMITTED f=1 buf=0x04000000 fmt=3 stride=512\n"
        )
        with mock.patch.dict(os.environ, {"SR_PERF": "1"}):
            status, report = self._run_case(launch_output=output)

        self.assertEqual(status, 0, report)
        self.assertEqual(report["presentation"], {
            "status": "FRAME_SUBMITTED",
            "frame_submissions": 1,
            "backend": "offscreen",
        })
        nk_cli.validate_bringup_report(report)

    def test_headless_bringup_overrides_inherited_sdl3_selectors(self):
        inherited = {
            "SDL_VIDEO_DRIVER": "windows",
            "SDL_VIDEODRIVER": "windows",
            "SDL_AUDIO_DRIVER": "wasapi",
            "SDL_AUDIODRIVER": "wasapi",
        }
        with mock.patch.dict(os.environ, inherited):
            status, report = self._run_case()

        self.assertEqual(status, 0, report)
        self.assertEqual(report["failure_class"], "NONE")
        for selector in inherited:
            self.assertEqual(self.last_launch_env.get(selector), "dummy")

    def test_headless_bringup_overrides_inherited_video_presenter(self):
        with mock.patch.dict(os.environ, {"SR_VIDEO": "windows"}):
            status, report = self._run_case()

        self.assertEqual(status, 0, report)
        self.assertEqual(report["failure_class"], "NONE")
        self.assertEqual(self.last_launch_env.get("SR_VIDEO"), "offscreen")

    def test_zero_exit_with_unrecognized_presenter_backend_is_not_success(self):
        output = (
            "BOOT_EVENT phase=window_ready backend=unknown-host\n"
            "BOOT_EVENT phase=frame_present backend=offscreen frame=1\n"
            "HOST_PRESENT_SUBMITTED f=1 buf=0x04000000 fmt=3 stride=512\n"
        )
        status, report = self._run_case(launch_output=output)

        self.assertEqual(status, 1)
        self.assertEqual(report["failure_class"], "NO_FRAME_SUBMISSIONS")
        self.assertEqual(report["presentation"], {
            "status": "NO_FRAME_SUBMISSIONS",
            "frame_submissions": 0,
            "backend": "unknown",
        })
        nk_cli.validate_bringup_report(report)

    def test_zero_exit_with_submission_before_frame_is_not_success(self):
        output = (
            "BOOT_EVENT phase=window_ready backend=offscreen\n"
            "HOST_PRESENT_SUBMITTED f=1 buf=0x04000000 fmt=3 stride=512\n"
            "BOOT_EVENT phase=frame_present backend=offscreen frame=1\n"
        )
        status, report = self._run_case(launch_output=output)

        self.assertEqual(status, 1)
        self.assertEqual(report["failure_class"], "NO_FRAME_SUBMISSIONS")
        self.assertEqual(report["presentation"], {
            "status": "NO_FRAME_SUBMISSIONS",
            "frame_submissions": 0,
            "backend": "offscreen",
        })
        nk_cli.validate_bringup_report(report)

    def test_zero_exit_with_malformed_frame_marker_is_not_success(self):
        output = (
            "BOOT_EVENT phase=window_ready backend=offscreen\n"
            "BOOT_EVENT phase=frame_present backend=offscreen BROKEN\n"
            "HOST_PRESENT_SUBMITTED f=1 buf=0x04000000 fmt=3 stride=512\n"
        )
        status, report = self._run_case(launch_output=output)

        self.assertEqual(status, 1)
        self.assertEqual(report["failure_class"], "NO_FRAME_SUBMISSIONS")
        self.assertEqual(report["presentation"], {
            "status": "NO_FRAME_SUBMISSIONS",
            "frame_submissions": 0,
            "backend": "offscreen",
        })
        nk_cli.validate_bringup_report(report)

    def test_zero_exit_with_framebuffer_setup_but_no_gui_submission_is_not_success(self):
        status, report = self._run_case(launch_output="synthetic runtime had no GUI submission\n")

        self.assertEqual(status, 1)
        self.assertEqual(report["presentation"], {
            "status": "NO_FRAME_SUBMISSIONS",
            "frame_submissions": 0,
            "backend": "unknown",
        })
        self.assertEqual(report["failure_class"], "NO_FRAME_SUBMISSIONS")
        self.assertIn(308, report["issue_numbers"])
        self.assertIn("no validated framebuffer was submitted",
                      nk_cli._bringup_human_summary(report))
        nk_cli.validate_bringup_report(report)

    def test_dummy_video_driver_message_does_not_mask_named_runtime_boundary(self):
        output = (
            "Vulkan support is not available in current SDL video driver (dummy)\n"
            "unsupported instruction: DIV.S at 0x08801234\n"
        )
        status, report = self._run_case(launch_code=1, launch_output=output)

        self.assertEqual(status, 1)
        self.assertEqual(report["runtime_output_kind"], "UNSUPPORTED_INSTRUCTION")
        self.assertEqual(report["failure_class"], "UNSUPPORTED_INSTRUCTION")
        self.assertIn(118, report["issue_numbers"])
        self.assertEqual(sorted(report["issue_numbers"]), [118, 308])

    def test_dummy_video_driver_message_remains_headless_fallback(self):
        output = "Vulkan support is not available in current SDL video driver (dummy)\n"
        status, report = self._run_case(launch_code=1, launch_output=output)

        self.assertEqual(status, 1)
        self.assertEqual(report["runtime_output_kind"], "VIDEO_UNAVAILABLE")
        self.assertEqual(report["failure_class"], "HEADLESS_UNAVAILABLE")

    def test_instruction_trace_is_opt_in_and_stays_under_private_work_dir(self):
        status, report = self._run_case(instruction_trace=True)

        self.assertEqual(status, 0, report)
        command = self.last_launch_command
        trace_path = Path(command[6])
        self.assertEqual(command[5], "none")
        self.assertTrue(self.last_build_arguments.instruction_trace)
        self.assertEqual(
            trace_path,
            self.root / "success" / "work" / "instructions.trace",
        )
        self.assertEqual(command[7:], ["--sched", "--gui"])
        self.assertNotIn(str(trace_path), json.dumps(report))

    def test_instruction_trace_uses_make_trace_flag(self):
        with mock.patch.dict(os.environ, {"TRACE": "0"}):
            env = nk_cli._runtime_build_environment(instruction_trace=True)

        self.assertEqual(env["TRACE"], "1")

    def test_zero_exit_before_first_hle_is_a_named_in_works_boundary(self):
        status, report = self._run_case(flight_events=[])

        self.assertEqual(status, 1)
        self.assertEqual(report["failure_class"], "EXITED_ZERO_BEFORE_HLE")
        self.assertEqual(report["exit_classification"], "EXITED_ZERO")
        self.assertEqual(report["stages"]["launch"]["status"], "FAIL")
        self.assertEqual(report["issue_numbers"], [308])
        summary = nk_cli._bringup_human_summary(report)
        self.assertIn("before its first PSP kernel import", summary)
        self.assertIn("related support is in the works", summary)
        self.assertNotRegex(summary, r"#[0-9]+")
        nk_cli.validate_bringup_report(report)

    def test_run_budget_detection_needs_the_runtime_budget_event(self):
        self.assertTrue(nk_cli._runtime_run_budget_ended(
            "BOOT_EVENT phase=window_ready backend=offscreen\n"
            "BOOT_EVENT phase=exit_at_vblank vblanks=6700 (SR_EXIT_AT_VBLANK=6700)\n"))
        self.assertFalse(nk_cli._runtime_run_budget_ended(
            "note: BOOT_EVENT phase=exit_at_vblank vblanks=6700 (SR_EXIT_AT_VBLANK=6700)"))
        self.assertFalse(nk_cli._runtime_run_budget_ended(""))
        self.assertFalse(nk_cli._runtime_run_budget_ended(None))

    def test_budget_ended_live_guest_is_not_labelled_as_an_exit(self):
        status, report = self._run_case(
            flight_events=[{"class": "hle", "kind": 1, "arg0": 0x446D8DE6}],
            launch_output=(
                "BOOT_EVENT phase=window_ready backend=offscreen\n"
                "BOOT_EVENT phase=exit_at_vblank vblanks=6700 (SR_EXIT_AT_VBLANK=6700)\n"
            ),
        )

        self.assertEqual(status, 1)
        self.assertEqual(report["failure_class"], "RUN_BUDGET_ENDED_BEFORE_FRAMEBUFFER_SETUP")
        self.assertEqual(report["exit_classification"], "RUN_BUDGET_ENDED")
        self.assertEqual(report["stages"]["launch"]["status"], "FAIL")
        self.assertEqual(report["issue_numbers"], [308])
        summary = nk_cli._bringup_human_summary(report)
        self.assertIn("run budget ended", summary)
        self.assertNotIn("exited zero", summary.casefold())
        nk_cli.validate_bringup_report(report)

    def test_budget_ended_before_any_hle_is_not_labelled_as_an_exit(self):
        status, report = self._run_case(
            flight_events=[],
            launch_output="BOOT_EVENT phase=exit_at_vblank vblanks=6700 (SR_EXIT_AT_VBLANK=6700)\n",
        )

        self.assertEqual(status, 1)
        self.assertEqual(report["failure_class"], "RUN_BUDGET_ENDED_BEFORE_FRAMEBUFFER_SETUP")
        self.assertEqual(report["exit_classification"], "RUN_BUDGET_ENDED")
        nk_cli.validate_bringup_report(report)

    def test_bounded_launch_output_keeps_both_ends_and_names_the_elision(self):
        text = "HEAD" + ("x" * 1000) + "TAIL"
        bounded = nk_cli._bounded_launch_output(text, 300)

        self.assertLessEqual(len(bounded), 300)
        self.assertTrue(bounded.startswith(b"HEAD"))
        self.assertTrue(bounded.endswith(b"TAIL"))
        self.assertIn(b"bytes elided", bounded)
        short = nk_cli._bounded_launch_output("small\n", 300)
        self.assertEqual(short, b"small\n")

    def test_bringup_keeps_the_launch_log_in_the_work_dir(self):
        self._run_case(launch_output="LAUNCH-LOG-MARKER\n")

        log = self.root / "success" / "work" / nk_cli.BRINGUP_LAUNCH_LOG_NAME
        self.assertIn(b"LAUNCH-LOG-MARKER", log.read_bytes())

    def test_bringup_launch_log_is_bounded(self):
        with mock.patch.object(nk_cli, "BRINGUP_LAUNCH_LOG_MAX_BYTES", 512):
            self._run_case(launch_output="y" * 4000 + "LAUNCH-LOG-END\n")

        log = self.root / "success" / "work" / nk_cli.BRINGUP_LAUNCH_LOG_NAME
        data = log.read_bytes()
        self.assertLessEqual(len(data), 512)
        self.assertIn(b"LAUNCH-LOG-END", data)

    def test_timed_out_launch_still_keeps_its_output(self):
        self._run_case(timeout=True, launch_output="TIMEOUT-LOG-MARKER\n")

        log = self.root / "success" / "work" / nk_cli.BRINGUP_LAUNCH_LOG_NAME
        self.assertIn(b"TIMEOUT-LOG-MARKER", log.read_bytes())

    def test_zero_exit_with_dropped_flight_events_is_unverified(self):
        status, report = self._run_case(flight_events=[], flight_dropped=1)

        self.assertEqual(status, 1)
        self.assertEqual(report["failure_class"], "GUEST_ACTIVITY_UNVERIFIED")
        self.assertEqual(report["exit_classification"], "EXITED_ZERO")
        self.assertEqual(report["issue_numbers"], [308])
        self.assertIn("telemetry did not verify a PSP kernel import",
                      nk_cli._bringup_human_summary(report))
        nk_cli.validate_bringup_report(report)

    def test_zero_exit_after_hle_before_framebuffer_setup_is_a_named_boundary(self):
        status, report = self._run_case(
            flight_events=[{"class": "hle", "kind": 1, "arg0": 0x446D8DE6}],
        )

        self.assertEqual(status, 1)
        self.assertEqual(report["failure_class"], "EXITED_ZERO_BEFORE_FRAMEBUFFER_SETUP")
        self.assertEqual(report["exit_classification"], "EXITED_ZERO")
        self.assertEqual(report["stages"]["launch"]["status"], "FAIL")
        self.assertEqual(report["issue_numbers"], [308])
        summary = nk_cli._bringup_human_summary(report)
        self.assertIn("before PSP display framebuffer setup", summary)
        self.assertIn("related support is in the works", summary)
        self.assertNotRegex(summary, r"#[0-9]+")
        nk_cli.validate_bringup_report(report)

    def test_zero_exit_after_self_unload_names_module_lifecycle_boundary(self):
        status, report = self._run_case(
            flight_events=[{"class": "hle", "kind": 1, "arg0": 0x8F2DF740}],
        )

        self.assertEqual(status, 1)
        self.assertEqual(report["failure_class"], "MODULE_SELF_UNLOAD_BEFORE_FRAMEBUFFER_SETUP")
        self.assertEqual(report["exit_classification"], "EXITED_ZERO")
        self.assertIn(280, report["issue_numbers"])
        self.assertEqual(sorted(report["issue_numbers"]), [280, 308])
        summary = nk_cli._bringup_human_summary(report)
        self.assertIn("unloaded itself before PSP display framebuffer setup", summary)
        self.assertIn("related support is in the works", summary)
        self.assertIn("still needs investigation", summary)
        self.assertNotRegex(summary, r"#[0-9]+")
        nk_cli.validate_bringup_report(report)

    def test_zero_exit_with_lost_framebuffer_evidence_is_display_unverified(self):
        status, report = self._run_case(
            flight_events=[{"class": "hle", "kind": 1, "arg0": 0x446D8DE6}],
            flight_dropped=1,
        )

        self.assertEqual(status, 1)
        self.assertEqual(report["failure_class"], "DISPLAY_PROGRESS_UNVERIFIED")
        self.assertEqual(report["stages"]["launch"]["status"], "FAIL")
        self.assertEqual(report["issue_numbers"], [308])
        self.assertIn("did not verify PSP display framebuffer setup",
                      nk_cli._bringup_human_summary(report))
        nk_cli.validate_bringup_report(report)

    def _run_module_fixture(
        self, iso_path: Path, work_root: Path, *,
        user_decrypted_eboot: bytes | None = None,
        user_decrypted_modules: dict[str, bytes] | None = None,
        catalog_manifest: dict | None = None,
        forbid_iso_executable: bool = False,
        package_modules: bool = False,
        launch_envs: list | None = None,
        user_manifest: dict | None = None,
        native_stager=None,
        package_build=None,
    ):
        work_dir = work_root / "work"
        report_path = work_root / "bringup.json"
        if user_manifest is not None:
            manifest_dir = work_dir / "user-data" / "manifests"
            manifest_dir.mkdir(parents=True, exist_ok=True)
            (manifest_dir / "user-title.json").write_text(
                json.dumps(user_manifest), encoding="utf-8"
            )
        if user_decrypted_modules:
            decrypted_dir = (
                work_dir / "user-data" / "titles" / "ULUS99998" / "decrypted"
            )
            decrypted_dir.mkdir(parents=True, exist_ok=True)
            for name, module_bytes in user_decrypted_modules.items():
                (decrypted_dir / name).write_bytes(module_bytes)
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
                return (
                    "BOOT_EVENT phase=window_ready backend=offscreen\n"
                    "BOOT_EVENT phase=frame_present backend=offscreen frame=1\n"
                    "HOST_PRESENT_SUBMITTED f=1 buf=0x04000000 fmt=3 stride=512\n"
                ), None

        def fake_package_build(build_args, stage_observer=None):
            self.last_build_module_dir = build_args.module_dir
            stage_observer("codegen", "START", 0)
            stage_observer("codegen", "PASS", 1)
            build_args.aot_stage_result = {"status": "REUSED", "reason": "NONE"}
            stage_observer("compile", "PASS", 1)
            package_dir = build_args.user_data_root / "packages" / "ULUS99998"
            package_dir.mkdir(parents=True, exist_ok=True)
            (package_dir / "runtime.exe").write_bytes(b"synthetic runtime")
            if package_modules:
                (package_dir / "modules").mkdir(exist_ok=True)
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

        def fake_popen(_command, **kwargs):
            if launch_envs is not None:
                launch_envs.append(dict(kwargs["env"]))
            self.last_launch_env = dict(kwargs["env"])
            Path(kwargs["env"]["SR_FLIGHT_OUTPUT"]).write_text(json.dumps({
                "recorder": {"dropped": 0},
                "events": [{"class": "hle", "kind": 1, "arg0": 0x289D82FE}],
            }), encoding="utf-8")
            return FakeProcess()

        completed = subprocess.CompletedProcess(["synthetic-codegen"], 0, "", "")
        with contextlib.ExitStack() as stack:
            if catalog_manifest is not None:
                inspect_iso = nk_cli.inspect_iso

                def inspect_catalog_iso(path, **kwargs):
                    metadata = inspect_iso(path, **kwargs)
                    profile = TitleProfile(
                        id=catalog_manifest["id"],
                        name=catalog_manifest.get("game_name", catalog_manifest["display_name"]),
                        disc_ids=[metadata.disc_id],
                        regions=[metadata.region],
                    )
                    return replace(metadata, matched_profile=profile)

                stack.enter_context(mock.patch.object(
                    nk_cli, "inspect_iso", side_effect=inspect_catalog_iso
                ))
                stack.enter_context(mock.patch.object(
                    nk_cli, "_find_public_manifest",
                    return_value=(self.root / "synthetic-manifest.json", catalog_manifest),
                ))
            stack.enter_context(mock.patch.object(
                nk_cli.subprocess, "run", return_value=completed
            ))
            stack.enter_context(mock.patch.object(
                nk_cli.subprocess, "Popen", side_effect=fake_popen
            ))
            stack.enter_context(mock.patch.object(
                nk_cli, "cmd_build_package", side_effect=package_build or fake_package_build
            ))
            if forbid_iso_executable:
                stack.enter_context(mock.patch.object(
                    nk_cli, "_extract_iso_executable",
                    side_effect=AssertionError("CFW loader was selected for analysis"),
                ))
            if native_stager is not None:
                stack.enter_context(mock.patch.object(
                    nk_cli, "NativeTitleStager", side_effect=native_stager
                ))
            with mock.patch("builtins.print"):
                return nk_cli.cmd_bringup(args), json.loads(
                    report_path.read_text(encoding="utf-8")
                )

    def test_multi_module_iso_modules_are_placed_by_the_guest_allocator(self):
        work_root = self.root / "multi-module-case"
        work_root.mkdir(parents=True)
        module_bytes = build_synthetic_decrypted_prx()
        main_bytes = build_synthetic_iso_elf()
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
        # No build-time address: each module is translated position-independently and
        # laid out by the guest allocator when the game loads it (#704).
        self.assertTrue(all(module["placement"] == "runtime" for module in modules))
        self.assertTrue(all("load_address" not in module for module in modules))
        self.assertTrue(all(module["required"] and module["role"] == "guest-prx" for module in modules))
        nk_cli.validate_bringup_report(report)

    def test_disc_whose_modules_the_runtime_serves_translates_no_guest_module(self):
        """A disc module the runtime replaces completely is left out of translation.

        When that leaves no module to place, the title continues exactly like a
        disc without modules instead of handing code generation a module
        folder with nothing selected from it.
        """
        work_root = self.root / "runtime-served-module-case"
        work_root.mkdir(parents=True)
        served_nids = sorted(runtime_registered_nids())[:4]
        iso_path = work_root / "runtime-served-module.iso"
        create_test_iso_with_modules(
            iso_path,
            build_synthetic_iso_elf(),
            sysdir_modules={},
            usrdir_modules={
                "served.prx": build_module_elf(
                    [SYSLIB_EXPORT, ("SynthServed", 0x0001, served_nids, [])]
                ),
            },
            disc_id="ULUS99998",
            title="Synthetic Runtime Served Module",
        )
        status, report = self._run_module_fixture(iso_path, work_root)

        self.assertEqual(status, 0, report)
        self.assertEqual(report["reached_stage"], "launch")
        self.assertEqual(report["failure_class"], "NONE")
        self.assertTrue(all(stage["status"] == "PASS" for stage in report["stages"].values()))
        self.assertEqual(report["counts"]["modules"], 1)
        profile_dir = work_root / "work" / "user-data" / "experimental" / "ULUS99998"
        profile = json.loads((profile_dir / "profile.json").read_text(encoding="utf-8"))
        self.assertEqual(profile["manifest"]["modules"], [])
        self.assertIsNone(self.last_build_module_dir)
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
        preflight = nk_cli.inspect_compatibility_preflight(
            iso_path,
            metadata=nk_cli.inspect_iso(iso_path),
            runtime_root=work_root / "inspect-user-data",
        )
        self.assertEqual(preflight["selected_executable_source"], "EBOOT.OLD")
        self.assertIsNone(preflight["selected_executable"])
        cfw_check = next(
            check for check in preflight["checks"]
            if check["code"] == "MODIFIED_DUMP_CFW_LOADER"
        )
        self.assertIn("EBOOT.OLD is the game executable", cfw_check["message"])
        with mock.patch("builtins.print") as printed:
            self.assertEqual(nk_cli.cmd_inspect(argparse.Namespace(
                iso=str(iso_path), root=str(work_root / "inspect-user-data"), json=False,
            )), 2)
        inspection_output = "\n".join(
            str(call.args[0]) for call in printed.call_args_list
        )
        self.assertIn("Game executable: EBOOT.OLD", inspection_output)
        summary = nk_cli._bringup_human_summary(report)
        self.assertIn("custom-firmware patch", summary)
        self.assertIn("EBOOT.OLD (encrypted)", summary)
        self.assertIn("decrypted/EBOOT.elf", summary)
        self.assertIn("does not provide decrypted executables", summary)
        self.assertIn("unmodified copy of your disc", summary)
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
        preflight = nk_cli.inspect_compatibility_preflight(
            iso_path,
            metadata=nk_cli.inspect_iso(iso_path),
            runtime_root=work_root / "work" / "user-data",
        )
        self.assertEqual(preflight["selected_executable_source"], "EBOOT.OLD")
        self.assertEqual(preflight["selected_executable"], "EBOOT.elf")
        self.assertEqual(
            nk_cli._psp_boot_path(preflight["selected_executable_source"]),
            "disc0:/PSP_GAME/SYSDIR/EBOOT.OLD",
        )
        self.assertIsNone(nk_cli._psp_boot_path("../outside.elf"))
        cfw_check = next(
            check for check in preflight["checks"]
            if check["code"] == "MODIFIED_DUMP_CFW_LOADER"
        )
        self.assertIn("EBOOT.OLD is the game executable", cfw_check["message"])
        self.assertIn("decrypted EBOOT.elf", cfw_check["message"])
        self.assertIn(
            "Custom-firmware-patched dump: using the original executable",
            cfw_check["message"],
        )
        self.assertIn("in the works.", cfw_check["message"])
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
        summary = nk_cli._bringup_human_summary(report)
        self.assertIn(
            "Custom-firmware-patched dump: using the original executable",
            summary,
        )
        self.assertIn("Broader CFW dump support is in the works.", summary)
        self.assertNotRegex(summary, r"#[0-9]+")
        library = json.loads(
            (work_root / "work" / "user-data" / "library.json")
            .read_text(encoding="utf-8")
        )
        self.assertEqual(library["games"][0]["selected_executable"], "EBOOT.BIN")
        self.assertEqual(library["games"][0]["boot_executable"], "EBOOT.OLD")
        nk_cli.validate_bringup_report(report)

    def test_relocatable_main_entry_is_rebased_to_guest_address(self):
        work_root = self.root / "relocatable-main-entry-case"
        work_root.mkdir(parents=True)
        iso_path = work_root / "relocatable-main-entry.iso"
        create_test_iso_with_modules(
            iso_path,
            build_synthetic_cfw_loader(),
            sysdir_modules={},
            usrdir_modules={},
            old_eboot=build_psp_container(),
            disc_id="ULUS99998",
            title="Synthetic Relocatable Main Entry",
        )

        status, report = self._run_module_fixture(
            iso_path,
            work_root,
            user_decrypted_eboot=build_synthetic_decrypted_prx(),
            forbid_iso_executable=True,
        )

        self.assertEqual(status, 0, report)
        profile = json.loads(
            (work_root / "work" / "user-data" / "experimental" / "ULUS99998" / "profile.json")
            .read_text(encoding="utf-8")
        )
        executable = profile["manifest"]["executable"]
        self.assertEqual(executable["base"], 0x08804000)
        self.assertEqual(executable["entry"], 0x08804000)

    def test_launch_points_the_runtime_at_the_packaged_modules(self):
        # The package ships its guest modules in <package>/modules; the bring-up
        # launch must hand the runtime that directory (as a player launch does),
        # never an inherited or development path.
        work_root = self.root / "packaged-modules-case"
        work_root.mkdir(parents=True)
        iso_path = work_root / "packaged-modules.iso"
        create_test_iso_with_modules(
            iso_path,
            bytes(build_synthetic_iso_elf()),
            sysdir_modules={"alpha.prx": build_synthetic_decrypted_prx()},
            usrdir_modules={},
            disc_id="ULUS99998",
            title="Synthetic Packaged Modules",
        )
        launch_envs: list = []
        with mock.patch.dict(os.environ, {"SR_MODULE_DIR": str(work_root / "inherited")}):
            status, report = self._run_module_fixture(
                iso_path, work_root, package_modules=True, launch_envs=launch_envs)
        self.assertEqual(status, 0, report)
        self.assertEqual(len(launch_envs), 1)
        package_dir = work_root / "work" / "user-data" / "packages" / "ULUS99998"
        self.assertEqual(launch_envs[0]["SR_MODULE_DIR"], str(package_dir / "modules"))

        launch_envs.clear()
        other_root = self.root / "unpackaged-modules-case"
        other_root.mkdir(parents=True)
        with mock.patch.dict(os.environ, {"SR_MODULE_DIR": str(other_root / "inherited")}):
            status, report = self._run_module_fixture(
                iso_path, other_root, launch_envs=launch_envs)
        self.assertEqual(status, 0, report)
        self.assertNotIn("SR_MODULE_DIR", launch_envs[0])

    def test_main_image_leaving_little_memory_still_plans_its_modules(self):
        # Nothing is reserved at build time: a main image that leaves little user
        # memory gets its modules planned, and whether one fits is the guest
        # allocator's answer when the game loads it (#704).
        work_root = self.root / "little-memory-case"
        work_root.mkdir(parents=True)
        iso_path = work_root / "little-memory.iso"
        create_test_iso_with_modules(
            iso_path,
            build_plain_mips_elf(e_type=2, vaddr=0x09E00000, memsz=0x10000),
            sysdir_modules={
                "module.prx": build_plain_mips_elf(e_type=0xFFA0, vaddr=0, memsz=0x100),
            },
            usrdir_modules={},
            disc_id="ULUS99998",
            title="Synthetic Little Memory",
        )
        status, report = self._run_module_fixture(iso_path, work_root)

        self.assertNotEqual(report["failure_class"], "GUEST_MODULE_LOAD_ADDRESS_LAYOUT_UNAVAILABLE")
        self.assertEqual(report["stages"]["prepare_import"]["status"], "PASS", report)
        profile = json.loads(
            (work_root / "work" / "user-data" / "experimental" / "ULUS99998" / "profile.json")
            .read_text(encoding="utf-8")
        )
        self.assertEqual(profile["manifest"]["modules"], [{
            "name": "module.prx", "required": True, "role": "guest-prx",
            "placement": "runtime", "guest_path": "disc0:/PSP_GAME/SYSDIR/module.prx",
        }])
        nk_cli.validate_bringup_report(report)

    def test_main_image_past_user_memory_is_named(self):
        work_root = self.root / "no-module-range-case"
        work_root.mkdir(parents=True)
        iso_path = work_root / "no-module-range.iso"
        create_test_iso_with_modules(
            iso_path,
            build_plain_mips_elf(e_type=2, vaddr=0x09FF0000, memsz=0x20000),
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
        self.assertEqual(report["issue_numbers"], [308])
        self.assertEqual(report["counts"]["modules"], 2)
        self.assertEqual(report["counts"]["encrypted_modules"], 1)
        nk_cli.validate_bringup_report(report)

    def test_user_decrypted_prx_replaces_encrypted_iso_module(self):
        work_root = self.root / "decrypted-module-case"
        work_root.mkdir(parents=True)
        module_bytes = build_synthetic_decrypted_prx()
        main_bytes = build_synthetic_iso_elf()
        iso_path = work_root / "encrypted-module.iso"
        create_test_iso_with_modules(
            iso_path,
            bytes(main_bytes),
            sysdir_modules={"encrypted.prx": build_psp_container()},
            usrdir_modules={"plain.elf": bytes(module_bytes)},
            disc_id="ULUS99998",
            title="Synthetic Decrypted Module",
        )

        status, report = self._run_module_fixture(
            iso_path,
            work_root,
            user_decrypted_modules={"encrypted.prx": bytes(module_bytes)},
        )

        self.assertEqual(status, 0, report)
        self.assertEqual(report["reached_stage"], "launch")
        self.assertEqual(report["failure_class"], "NONE")
        self.assertEqual(report["counts"]["modules"], 2)
        self.assertEqual(report["counts"]["encrypted_modules"], 1)
        module_stage = next(
            (work_root / "work" / "user-data" / "experimental" / "ULUS99998")
            .glob("module-stage-*"),
        )
        self.assertEqual((module_stage / "encrypted.prx").read_bytes(), bytes(module_bytes))
        profile = json.loads(
            (work_root / "work" / "user-data" / "experimental" / "ULUS99998" / "profile.json")
            .read_text(encoding="utf-8")
        )
        self.assertEqual([module["name"] for module in profile["manifest"]["modules"]],
                         ["encrypted.prx", "plain.elf"])
        nk_cli.validate_bringup_report(report)

    def test_invalid_user_decrypted_prx_stays_at_crypto_boundary(self):
        work_root = self.root / "invalid-decrypted-module-case"
        work_root.mkdir(parents=True)
        iso_path = work_root / "encrypted-module.iso"
        create_test_iso_with_modules(
            iso_path,
            build_plain_mips_elf(e_type=2),
            sysdir_modules={"encrypted.prx": build_psp_container()},
            usrdir_modules={},
            disc_id="ULUS99998",
            title="Synthetic Invalid Decrypted Module",
        )

        status, report = self._run_module_fixture(
            iso_path,
            work_root,
            user_decrypted_modules={"encrypted.prx": build_psp_container()},
        )

        self.assertEqual(status, 1)
        self.assertEqual(report["reached_stage"], "prepare_import")
        # A still-encrypted "decrypted" copy is not a usable module: with no key
        # file the module still needs the decryption boundary (#308).
        self.assertEqual(report["failure_class"], "GUEST_MODULE_DECRYPTION_REQUIRED")
        self.assertEqual(report["issue_numbers"], [308])
        nk_cli.validate_bringup_report(report)

    def test_catalog_bringup_stages_user_decrypted_prx(self):
        work_root = self.root / "catalog-decrypted-module-case"
        work_root.mkdir(parents=True)
        module_bytes = build_synthetic_decrypted_prx()
        main_bytes = build_synthetic_iso_elf()
        iso_path = work_root / "catalog-encrypted-module.iso"
        create_test_iso_with_modules(
            iso_path,
            bytes(main_bytes),
            sysdir_modules={"encrypted.prx": build_psp_container()},
            usrdir_modules={},
            disc_id="ULUS99998",
            title="Synthetic Catalog Decrypted Module",
        )
        catalog_manifest = nk_cli.title_manifest.load_manifest(
            ROOT / "assets" / "titles" / "synthetic-title2.json"
        )
        catalog_manifest["executable"]["base"] = 0x08804000
        catalog_manifest["executable"]["entry"] = 0x08804000
        catalog_manifest["modules"] = [{
            "name": "encrypted.prx",
            "load_address": 0x08810000,
            "required": True,
            "role": "guest-prx",
            "guest_path": "disc0:/PSP_GAME/SYSDIR/encrypted.prx",
        }]
        catalog_manifest = nk_cli.title_manifest.validate_manifest(catalog_manifest)

        status, report = self._run_module_fixture(
            iso_path,
            work_root,
            user_decrypted_modules={"encrypted.prx": bytes(module_bytes)},
            catalog_manifest=catalog_manifest,
        )

        self.assertEqual(status, 0, report)
        self.assertEqual(report["reached_stage"], "launch")
        self.assertEqual(report["failure_class"], "NONE")
        self.assertEqual(report["stages"]["prepare_import"]["status"], "PASS")
        self.assertEqual(report["stages"]["codegen"]["status"], "PASS")
        self.assertEqual(report["stages"]["build_package"]["status"], "PASS")
        staged_module = (
            work_root / "work" / "user-data" / "cache" / "bringup" /
            "ULUS99998" / "modules" / "encrypted.prx"
        )
        self.assertEqual(staged_module.read_bytes(), bytes(module_bytes))
        nk_cli.validate_bringup_report(report)

    def _archive_bringup_fixture(self, case: str):
        """A source-owned archive disc and its user manifest, as the sweep stages them."""
        from test_iso_parity import archive_title_manifest, create_archive_title_iso

        work_root = self.root / case
        work_root.mkdir(parents=True)
        iso_path = work_root / "archive.iso"
        create_archive_title_iso(iso_path, disc_id="ULUS99998", title="Synthetic Archive",
                                 executable=bytes(build_synthetic_iso_elf()))
        # No game_name, as in a typical user manifest: the package naming rule
        # (the title id) applies.
        manifest = archive_title_manifest("ULUS99998", "archive-ulus99998")
        manifest["executable"]["base"] = 0x08804000
        manifest["executable"]["entry"] = 0x08804000
        manifest["executable"]["bss_metadata_source"] = "elf"
        return work_root, iso_path, manifest

    def test_archive_disc_bringup_stages_through_the_player_and_launches_from_it(self):
        """Failing-before: bring-up never staged an archive disc's files.

        A title known only from the user's manifest, whose data ships in archives,
        had its SR_DATAROOT resolved inside the source tree, where the disc's
        data never is. Bring-up now loads the user's manifests, sets the files up
        through the player's staging transaction in its own user-data root, and
        launches with the staged data root and loose-content anchor.
        """
        from nk_core import PreparationResult

        work_root, iso_path, manifest = self._archive_bringup_fixture("archive-bringup")
        user_root = work_root / "work" / "user-data"
        staged = user_root / "games" / "ULUS99998"
        calls = []

        def stager_factory(root, *args, **kwargs):
            def stage(iso, disc_id, progress):
                calls.append((Path(root), Path(iso), disc_id))
                (staged / "xbdata").mkdir(parents=True, exist_ok=True)
                return PreparationResult(success=True, disc_id=disc_id, prepared_root=staged)
            return stage

        status, report = self._run_module_fixture(
            iso_path, work_root, user_manifest=manifest, native_stager=stager_factory,
        )

        self.assertEqual(status, 0, report)
        self.assertEqual(report["failure_class"], "NONE")
        self.assertEqual(report["stages"]["prepare_import"]["status"], "PASS")
        self.assertEqual(len(calls), 1)
        root, iso, disc_id = calls[0]
        self.assertEqual(root.resolve(), user_root.resolve())
        self.assertEqual(iso, iso_path.resolve())
        self.assertEqual(disc_id, "ULUS99998")
        env = self.last_launch_env
        self.assertEqual(Path(env["SR_DATAROOT"]).resolve(), (staged / "xbdata").resolve())
        self.assertIn(str(staged.resolve()), env["SR_LOOSE_CONTENT_ROOTS"])
        nk_cli.validate_bringup_report(report)

    def test_bringup_codegen_reads_bss_from_the_disc_psp_header(self):
        """Failing-before: a psp-header manifest stopped bring-up at codegen.

        build-package reads the disc's own ~PSP header when the manifest takes
        BSS metadata from it, but bring-up's code generation did not, so such a
        title (the encrypted-executable case, with a user-supplied EBOOT.elf)
        failed with "psp_header is required" hidden behind CODEGEN_FAILED.
        Bring-up's codegen stage now runs inside the package build, through the
        same planner command as the package itself, so it is handed the disc
        header the package build resolves.
        """
        from nk_core import PreparationResult
        from test_iso_parity import archive_title_manifest, create_archive_title_iso

        work_root = self.root / "archive-psp-header"
        work_root.mkdir(parents=True)
        iso_path = work_root / "archive.iso"
        create_archive_title_iso(iso_path, disc_id="ULUS99998", title="Synthetic Archive",
                                 executable=build_psp_container())
        manifest = archive_title_manifest("ULUS99998", "archive-ulus99998")
        manifest["executable"]["base"] = 0x08804000
        manifest["executable"]["entry"] = 0x08804000
        manifest["executable"]["bss_metadata_source"] = "psp-header"
        staged = work_root / "work" / "user-data" / "games" / "ULUS99998"
        real_package_build = nk_cli.cmd_build_package
        planner_commands = []

        def stager_factory(root, *args, **kwargs):
            def stage(iso, disc_id, progress):
                (staged / "xbdata").mkdir(parents=True, exist_ok=True)
                return PreparationResult(success=True, disc_id=disc_id, prepared_root=staged)
            return stage

        def record_planner(command, **_kwargs):
            command = [str(part) for part in command]
            if not any(part.endswith("title_codegen_plan.py") for part in command):
                return subprocess.CompletedProcess(command, 0, "", "")
            planner_commands.append(command)
            # The stage succeeds; the package compile is refused so no package is needed.
            code = 0 if "--aot-stage" in command else 2
            return subprocess.CompletedProcess(command, code, "", "")

        def package_build(build_args, stage_observer=None):
            with mock.patch.object(nk_cli.subprocess, "run", side_effect=record_planner):
                return real_package_build(build_args, stage_observer=stage_observer)

        status, report = self._run_module_fixture(
            iso_path, work_root, user_manifest=manifest, native_stager=stager_factory,
            user_decrypted_eboot=bytes(build_synthetic_iso_elf()),
            package_build=package_build,
        )

        self.assertEqual(report["stages"]["codegen"]["status"], "PASS", report)
        self.assertEqual(report["stages"]["compile"]["status"], "FAIL", report)
        self.assertEqual(status, 1, report)
        stage, package = planner_commands
        self.assertIn("--aot-stage", stage)
        self.assertEqual(stage[stage.index("--output-dir") + 1],
                         str(work_root.resolve() / "work" / "codegen-stage"))
        header = Path(stage[stage.index("--psp-header") + 1])
        self.assertEqual(header.read_bytes()[:4], b"~PSP")
        # The package build is the same planner command, pointed at the stage.
        self.assertEqual(package[package.index("--psp-header") + 1], str(header))
        self.assertEqual(package[package.index("--reuse-aot-stage") + 1],
                         stage[stage.index("--output-dir") + 1])
        nk_cli.validate_bringup_report(report)

    def test_archive_disc_staging_failure_is_a_named_bringup_boundary(self):
        from nk_core import PreparationResult

        work_root, iso_path, manifest = self._archive_bringup_fixture("archive-bringup-fail")
        def stager_factory(root, *args, **kwargs):
            def stage(iso, disc_id, progress):
                return PreparationResult(
                    success=False, disc_id=disc_id, error_code="STAGE_DATA_FOLDER_MISSING",
                    error_message="[STAGE_DATA_FOLDER_MISSING] This disc image has no "
                                  "'xbdata' folder, which this game needs.",
                )
            return stage

        status, report = self._run_module_fixture(
            iso_path, work_root, user_manifest=manifest, native_stager=stager_factory,
        )

        self.assertEqual(status, 1)
        self.assertEqual(report["reached_stage"], "prepare_import")
        self.assertEqual(report["failure_class"], "DISC_FILES_STAGE_FAILED")
        self.assertEqual(report["stages"]["analyze"]["status"], "NOT_RUN")
        summary = nk_cli._bringup_human_summary(report)
        self.assertIn("could not be set up", summary)
        self.assertNotIn("xbdata", json.dumps(report))
        nk_cli.validate_bringup_report(report)

    def test_each_stage_failure_is_named_in_the_report(self):
        expected = {
            "inspect": "INVALID_ISO",
            "prepare_import": "EXPERIMENTAL_IMPORT_FAILED",
            "analyze": "ANALYZER_FAILURE_UNCLASSIFIED",
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
                if stage == "analyze":
                    summary = nk_cli._bringup_human_summary(report)
                    self.assertIn("ANALYZER_FAILURE_UNCLASSIFIED", summary)
                    self.assertIn("not supported yet", summary)
                    self.assertNotRegex(summary, r"#[0-9]+")
                nk_cli.validate_bringup_report(report)

    def test_analyzer_import_boundary_is_named_and_says_not_supported_yet(self):
        boundary = title_codegen_plan.PackageRouteError(
            "ANALYZER_IMPORT_NID_TABLE_MISSING",
            "synthetic import entry has no NID table",
        )
        status, report = self._run_case("analyze", analyzer_error=boundary)

        self.assertNotEqual(status, 0)
        self.assertEqual(report["reached_stage"], "analyze")
        self.assertEqual(report["failure_class"], "ANALYZER_IMPORT_NID_TABLE_MISSING")
        summary = nk_cli._bringup_human_summary(report)
        self.assertIn("ANALYZER_IMPORT_NID_TABLE_MISSING", summary)
        self.assertIn("not supported yet", summary)
        self.assertNotRegex(summary, r"#[0-9]+")
        sidecar = json.loads(
            (self.root / "analyze" / "work" / "sweep-imports.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertTrue(sidecar["analyzer_diagnostic"].startswith(
            "ANALYZER_IMPORT_NID_TABLE_MISSING: PackageRouteError:"
        ))
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
        self.assertIn("related support is in the works", summary)
        self.assertIn(308, report["issue_numbers"])
        self.assertNotRegex(summary, r"#[0-9]+")
        self.assertNotIn("#71", summary)
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
        self.assertIn("related support is in the works", summary)
        self.assertNotRegex(summary, r"#[0-9]+")
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
            launch_output="cannot open C:/synthetic/private-title.elf\n",
        )
        self.assertNotEqual(status, 0)
        self.assertEqual(report["failure_class"], "RUNTIME_INPUT_UNAVAILABLE")
        self.assertEqual(report["runtime_output_kind"], "DRIVER_INPUT_READ_FAILURE")
        self.assertNotIn("synthetic/private-title", json.dumps(report))
        summary = nk_cli._bringup_human_summary(report)
        self.assertIn("related support is in the works", summary)
        self.assertNotRegex(summary, r"#[0-9]+")
        nk_cli.validate_bringup_report(report)

    def test_runtime_trace_failure_hides_the_path(self):
        status, report = self._run_case(
            "launch",
            launch_code=2,
            launch_output="no '# init' in C:/synthetic/reference.trace\n",
        )
        self.assertNotEqual(status, 0)
        self.assertEqual(report["failure_class"], "RUNTIME_TRACE_UNAVAILABLE")
        self.assertEqual(report["runtime_output_kind"], "DRIVER_TRACE_INPUT_FAILURE")
        self.assertNotIn("reference.trace", json.dumps(report))
        summary = nk_cli._bringup_human_summary(report)
        self.assertIn("related support is in the works", summary)
        self.assertNotRegex(summary, r"#[0-9]+")
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
        summary = nk_cli._bringup_human_summary(report)
        self.assertIn("related support is in the works", summary)
        self.assertNotRegex(summary, r"#[0-9]+")
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
        self.assertIn("related support is in the works", summary)
        self.assertNotRegex(summary, r"#[0-9]+")
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

    def test_illegal_opcode_key_never_crashes_the_route_or_loses_the_report(self):
        status, report = self._run_case(unsupported_opcodes={"REGIMM-OTHER": 1})
        self.assertNotEqual(status, 0)
        nk_cli.validate_bringup_report(report)
        self.assertEqual(report["reached_stage"], "launch")
        self.assertEqual(report["counts"]["unsupported_opcodes"], {})
        self.assertTrue(
            all(stage["status"] == "PASS" for stage in report["stages"].values())
        )


class TestBringupReportOpcodeNames(unittest.TestCase):
    """A bring-up report must always land, with schema-legal opcode keys.

    The library sweep classifies a title from that report, so a report rejected by
    its own schema hides the stage and class the run reached.  The
    ``counts.unsupported_opcodes`` keys come from analyzer opcode identities, and
    some identities (``regimm-other``) carry characters the schema's
    ``^[A-Z][A-Z0-9_.]*$`` key pattern forbids.
    """

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="nk-bringup-opcodes-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def _key_pattern(self) -> str:
        schema = json.loads(nk_cli.BRINGUP_SCHEMA_PATH.read_text(encoding="utf-8"))
        return (
            schema["properties"]["counts"]["properties"]["unsupported_opcodes"]
            ["propertyNames"]["pattern"]
        )

    def test_every_cfg_opcode_identity_normalizes_to_a_schema_legal_name(self):
        pattern = self._key_pattern()
        names = set()
        for op in range(64):
            rs_values = range(32) if op in (0x11, 0x12) else range(1)
            for rs in rs_values:
                for rt in range(32):
                    for funct in range(64):
                        word = (op << 26) | (rs << 21) | (rt << 16) | funct
                        mnemonic = analyze._cfg_opcode_identity(word)["mnemonic"]
                        name = nk_cli._opcode_identity_report_name(mnemonic)
                        if re.fullmatch(pattern, name) is None:
                            self.fail(
                                f"opcode identity {mnemonic!r} reports as {name!r}, "
                                f"which is outside {pattern}"
                            )
                        names.add(name)
        self.assertIn("REGIMM_OTHER", names)
        self.assertIn("OP_3F", names)

    def test_regimm_other_opcode_word_produces_a_valid_report(self):
        word = 0x041F0000  # REGIMM with an unhandled rt: identity "regimm-other"

        class FakeElf:
            def read_at_vaddr(self, address, size):
                return word.to_bytes(4, "little")

        sources = [{
            "name": "executable region",
            "ranges": [(0x08800000, 0x08800040)],
            "elf": FakeElf(),
        }]
        stubs = self.root / "synthetic_recomp_stubs.txt"
        stubs.write_text("08800000 unsupported opcode at 0x08800004\n", encoding="ascii")

        counts = nk_cli._count_unsupported_opcodes(stubs, sources)
        self.assertEqual(counts, {"REGIMM_OTHER": 1})

        report = nk_cli._new_bringup_report()
        report["counts"]["unsupported_opcodes"] = counts
        report_path = self.root / "bringup.json"
        self.assertTrue(nk_cli._write_bringup_report(report, report_path))
        written = json.loads(report_path.read_text(encoding="utf-8"))
        nk_cli.validate_bringup_report(written)
        self.assertEqual(written["counts"]["unsupported_opcodes"], {"REGIMM_OTHER": 1})

    def test_forced_schema_violation_still_writes_a_valid_minimal_report(self):
        report = nk_cli._new_bringup_report()
        report["counts"]["functions"] = 4
        report["counts"]["unsupported_opcodes"] = {"REGIMM-OTHER": 1}
        nk_cli._fail_bringup(report, "codegen", "CODEGEN_FAILED", [308], 12)
        report_path = self.root / "schema-invalid.json"
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            written = nk_cli._write_bringup_report(report, report_path)

        self.assertFalse(written)
        message = stdout.getvalue()
        self.assertIn("REPORT_SCHEMA_INVALID", message)
        self.assertIn("report.counts.unsupported_opcodes property name", message)
        payload = json.loads(report_path.read_text(encoding="utf-8"))
        nk_cli.validate_bringup_report(payload)
        self.assertEqual(payload["reached_stage"], "codegen")
        self.assertEqual(payload["stages"]["codegen"], {"status": "FAIL", "duration_ms": 12})
        self.assertEqual(payload["failure_class"], "CODEGEN_FAILED")
        self.assertEqual(payload["issue_numbers"], [308])

    def test_unknown_failure_class_is_named_and_replaced_by_a_stage_class(self):
        report = nk_cli._new_bringup_report()
        nk_cli._fail_bringup(report, "launch", "SYNTHETIC_UNREPRESENTABLE_CLASS", [297], 5)
        report_path = self.root / "unknown-class.json"
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            written = nk_cli._write_bringup_report(report, report_path)

        self.assertFalse(written)
        self.assertIn("REPORT_SCHEMA_INVALID", stdout.getvalue())
        self.assertIn("SYNTHETIC_UNREPRESENTABLE_CLASS", stdout.getvalue())
        payload = json.loads(report_path.read_text(encoding="utf-8"))
        nk_cli.validate_bringup_report(payload)
        self.assertEqual(payload["reached_stage"], "launch")
        self.assertEqual(payload["stages"]["launch"]["status"], "FAIL")
        self.assertEqual(payload["failure_class"], "LAUNCH_FAILED")


class TestProductStatusCopy(unittest.TestCase):
    """Product status copy must match what the shipped build actually does.

    The player builds runtime packages from the library (#465/#469) and the
    clean-room public PGF reader landed in #474, closing #349, so wizard,
    checklist and completion-manifest text that still described those as
    missing was a false product claim. Boundaries that really are unavailable
    stay named with their tracking issue: PGD-protected data and no-keyfile
    executable decryption (#308).
    """

    def _source(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_wizard_names_the_package_builder_and_the_open_boundaries(self):
        ui = self._source("src/player/ui_renderer.c")
        self.assertIn("decrypts encrypted executables with a local key file. It builds runtime ", ui)
        self.assertIn("packages from the library. Verify also lists font and audio status.", ui)
        self.assertNotIn("or create runtime packages", ui)
        self.assertNotIn("draw_issue_links", ui)
        self.assertEqual(ui.count("Build the runtime package from the library."), 3)

    def test_library_checklist_points_at_the_library_package_builder(self):
        inspect = self._source("tools/nk_core/iso_inspect.py")
        self.assertIn("build it from the library.", inspect)

    def test_public_build_limits_report_the_font_reader_as_available(self):
        cli_source = self._source("tools/nk_cli.py")
        self.assertIn(
            "Fonts: import your own PSP fonts; the public PGF reader is "
            "available for supported inputs.",
            cli_source,
        )
        self.assertIn(
            "PGD-protected data: unavailable; broader ISO-to-Play support is in the works.",
            cli_source,
        )
        report_source = self._source("tools/title_codegen_plan.py")
        self.assertIn(
            "fonts: import your own PSP fonts; the public PGF reader is "
            "available for supported inputs (#474)",
            report_source,
        )
        self.assertIn(
            "PGD-protected data: unavailable; broader ISO-to-Play support is in the works (#308)",
            report_source,
        )
        self.assertNotIn("public font reader in the works (#349)", report_source)


if __name__ == "__main__":
    unittest.main()
