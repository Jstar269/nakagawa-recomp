# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""FSR 3's C port checked against the files it depends on.

* fsr3_pass_layout (src/rt/fsr3_host.c) against each embedded module's own decorations: every
  descriptor's binding, kind and name, which SRVs are sampled through s_LinearClamp, the local size,
  and the constant buffers' member order and offsets against the C structs in fsr3_host.h.
* The storage images that declare a format are exactly the atomic ones, as R32ui (the LOCAL DELTA).
* The derived C files carry the GPL header, AMD's MIT notice and a dated modification note.
* The mutant campaigns' texts each match their file exactly once.
* Inert (F-A-lite, static half): no FSR 3 file is compiled into the runtime or the player, and no
  runtime or player source refers to one.

No GPU, no DXC and no game data.
"""
from __future__ import annotations

from pathlib import Path
import re
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import fsr3_mutants  # noqa: E402
import shader_embed  # noqa: E402

HOST_C = ROOT / "src/rt/fsr3_host.c"
HOST_H = ROOT / "src/rt/fsr3_host.h"
GPU_DIR = ROOT / "src/rt/gpu_sdl3vk"

OP_NAME, OP_MEMBER_NAME, OP_EXECUTION_MODE, OP_TYPE_IMAGE, OP_TYPE_SAMPLER = 5, 6, 16, 25, 26
OP_TYPE_STRUCT, OP_TYPE_POINTER, OP_VARIABLE, OP_LOAD, OP_IMAGE_TEXEL_POINTER = 30, 32, 59, 61, 60
OP_SAMPLED_IMAGE, OP_DECORATE, OP_MEMBER_DECORATE = 86, 71, 72
DEC_BINDING, DEC_DESCRIPTOR_SET, DEC_OFFSET = 33, 34, 35
MODE_LOCAL_SIZE, FORMAT_UNKNOWN, FORMAT_R32UI = 17, 0, 33


def spirv_words(inc: Path) -> list[int]:
    data = shader_embed.parse_embedding(inc.read_text(encoding="ascii"))
    return list(struct.unpack(f"<{len(data) // 4}I", data))


def _string(words: list[int]) -> str:
    return b"".join(w.to_bytes(4, "little") for w in words).split(b"\0", 1)[0].decode()


def reflect(words: list[int]) -> dict:
    """Bindings, local size, cbuffer members and image formats of one compute module."""
    if words[0] != 0x07230203:
        raise ValueError("not SPIR-V")
    names, binding, dset, types, pointers, variables = {}, {}, {}, {}, {}, {}
    member_names, offsets, loads, sampled, texel_pointers = {}, {}, {}, set(), set()
    local = None
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xFFFF
        a = words[i + 1:i + count]
        if op == OP_NAME:
            names[a[0]] = _string(a[1:])
        elif op == OP_MEMBER_NAME:
            member_names[(a[0], a[1])] = _string(a[2:])
        elif op == OP_DECORATE and a[1] == DEC_BINDING:
            binding[a[0]] = a[2]
        elif op == OP_DECORATE and a[1] == DEC_DESCRIPTOR_SET:
            dset[a[0]] = a[2]
        elif op == OP_MEMBER_DECORATE and a[2] == DEC_OFFSET:
            offsets[(a[0], a[1])] = a[3]
        elif op == OP_EXECUTION_MODE and a[1] == MODE_LOCAL_SIZE:
            local = tuple(a[2:5])
        elif op == OP_TYPE_IMAGE:
            types[a[0]] = ("image", a[6], a[7])  # Sampled, Image Format
        elif op == OP_TYPE_SAMPLER:
            types[a[0]] = ("sampler",)
        elif op == OP_TYPE_STRUCT:
            types[a[0]] = ("struct", len(a) - 1)
        elif op == OP_TYPE_POINTER:
            pointers[a[0]] = a[2]
        elif op == OP_VARIABLE:
            variables[a[1]] = a[0]
        elif op == OP_LOAD:
            loads[a[1]] = a[2]
        elif op == OP_SAMPLED_IMAGE:
            sampled.add(a[2])
        elif op == OP_IMAGE_TEXEL_POINTER:
            texel_pointers.add(a[2])
        i += count
    rows, formats, cbuffers = [], {}, {}
    for var, number in binding.items():
        pointee = pointers[variables[var]]
        kind = types.get(pointee, ("other",))
        name = names.get(var, "")
        if kind[0] == "image":
            what = "SRV" if kind[1] == 1 else "UAV"
            formats[name] = (kind[2], var in texel_pointers)
        elif kind[0] == "sampler":
            what = "SAMPLER"
        else:
            what = "CBV"
            cbuffers[name] = [(member_names.get((pointee, m)), offsets.get((pointee, m))) for m in range(kind[1])]
        linear = any(loads.get(image) == var for image in sampled)
        rows.append((number, dset.get(var), what, name, linear))
    return {"local": local, "rows": sorted(rows), "formats": formats, "cbuffers": cbuffers}


def c_tables() -> dict[str, list[tuple[int, str, str, bool]]]:
    text = HOST_C.read_text(encoding="utf-8")
    tables = {}
    for name, body in re.findall(r"static const Fsr3Binding (\w+)\[\] = \{\n(.*?)\n\};", text, re.S):
        rows = re.findall(r'\{ (\d+), FSR3_BIND_(\w+), [^,]+, ([01]), "(\w+)" \}', body)
        if len(rows) != body.count("{"):
            raise ValueError(f"{name}: unreadable row")
        tables[name] = [(int(b), kind, n, linear == "1") for b, kind, linear, n in rows]
    return tables


def c_layouts() -> list[tuple[str, int, int, str]]:
    text = HOST_C.read_text(encoding="utf-8")
    return [(label, int(x), int(y), table)
            for label, x, y, table in re.findall(r'LAYOUT\("([\w-]+)", (\d+), (\d+), (\w+)\)', text)]


def c_struct_fields(name: str) -> list[tuple[str, int]]:
    """(field, bytes) of a typedef'd struct of 4-byte scalars and arrays in fsr3_host.h."""
    text = HOST_H.read_text(encoding="utf-8")
    body = re.search(r"typedef struct \{\n([^{}]*)\n\} " + name + ";", text)
    if not body:
        raise ValueError(f"no struct {name}")
    fields = []
    for _kind, field, count in re.findall(r"^\s*(int32_t|uint32_t|float) (\w+)(?:\[(\d+)\])?;", body.group(1), re.M):
        fields.append((field, 4 * int(count or 1)))
    return fields


# HLSL member names that are not the C field's name with its type prefix dropped.
HLSL_RENAMED = {"fJitter": "jitterOffset", "fPreviousFrameJitter": "previousFrameJitterOffset",
                "fJitterSequenceLength": "jitterPhaseCount",
                "numWorkGroups": "numworkGroups"}  # AMD's C++ struct spells it so (ffx_fsr3upscaler.cpp:145)
CBUFFER_STRUCT = {"cbFSR3Upscaler": "Fsr3Constants", "cbSPD": "Fsr3SpdConstants", "cbRCAS": "Fsr3RcasConstants"}


def c_name(hlsl: str) -> str:
    if hlsl in HLSL_RENAMED:
        return HLSL_RENAMED[hlsl]
    stripped = re.sub(r"^[ifu](?=[A-Z])", "", hlsl)
    return stripped[0].lower() + stripped[1:]


def module_for(table: str) -> Path:
    return GPU_DIR / f"fsr3_{table}_comp.inc"


class BindingTableTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tables = c_tables()
        self.layouts = c_layouts()

    def test_every_pass_has_a_table_and_a_module(self) -> None:
        self.assertEqual(len(self.layouts), 8)
        embedded = sorted(GPU_DIR.glob("fsr3_*_comp.inc"))
        self.assertEqual(sorted(module_for(t) for *_, t in self.layouts), embedded)
        manifest = {Path(e).name for _, _, e in shader_embed.FSR3_SHADERS}
        self.assertEqual(manifest, {p.name for p in embedded})

    def test_bindings_kinds_names_and_linear_sampling_match_the_spirv(self) -> None:
        for _, _, _, table in self.layouts:
            with self.subTest(table=table):
                facts = reflect(spirv_words(module_for(table)))
                spirv = [(b, kind, name, linear) for b, s, kind, name, linear in facts["rows"]]
                self.assertTrue(all(s == 0 for _, s, *_ in facts["rows"]), "every descriptor is in set 0")
                self.assertEqual(self.tables[table], spirv)

    def test_local_sizes_match_the_spirv(self) -> None:
        for _, x, y, table in self.layouts:
            with self.subTest(table=table):
                self.assertEqual(reflect(spirv_words(module_for(table)))["local"], (x, y, 1))

    def test_constant_buffers_match_the_c_structs(self) -> None:
        seen = set()
        for _, _, _, table in self.layouts:
            for cb, members in reflect(spirv_words(module_for(table)))["cbuffers"].items():
                with self.subTest(table=table, cbuffer=cb):
                    fields = c_struct_fields(CBUFFER_STRUCT[cb])
                    self.assertEqual([c_name(m) for m, _ in members], [f for f, _ in fields])
                    offset = 0
                    for (member, spirv_offset), (_, size) in zip(members, fields, strict=True):
                        self.assertEqual(spirv_offset, offset, member)
                        offset += size
                    seen.add(cb)
        self.assertEqual(seen, set(CBUFFER_STRUCT))

    def test_only_the_atomic_images_declare_a_format_and_it_is_r32ui(self) -> None:
        atomics = set()
        for _, _, _, table in self.layouts:
            for name, (image_format, atomic) in reflect(spirv_words(module_for(table)))["formats"].items():
                with self.subTest(table=table, image=name):
                    self.assertEqual(image_format, FORMAT_R32UI if atomic else FORMAT_UNKNOWN)
                    if atomic:
                        atomics.add(name)
        self.assertEqual(atomics, {"rw_spd_global_atomic", "rw_reconstructed_previous_nearest_depth"})

    def test_the_manifest_pins_the_shader_options(self) -> None:
        for define in ("FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=1", "FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=0",
                       "FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=0",
                       "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2", "FFX_SPD_NO_WAVE_OPERATIONS=1",
                       "FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=0"):
            self.assertIn(define, shader_embed.FSR3_DEFINES)


class NoticeTests(unittest.TestCase):
    DERIVED = ("src/rt/fsr3_host.c", "src/rt/fsr3_host.h", "src/rt/fsr3_jitter.c", "src/rt/fsr3_jitter.h",
               "src/rt/gpu_sdl3vk/fsr3_pass.c")
    PROJECT = ("src/rt/gpu_sdl3vk/fsr3_pass.h", "src/rt/gpu_sdl3vk/fsr3_pass_selftest.c",
               "tests/native/fsr3_host_selftest.c", "tools/fsr3_mutants.py")

    def test_derived_files_keep_amds_notice_and_say_they_were_modified(self) -> None:
        licence = (ROOT / "third_party/fidelityfx_sdk/LICENSE-AMD.md").read_text(encoding="utf-8")
        self.assertIn("Permission is hereby granted, free of charge", licence)
        for rel in self.DERIVED:
            with self.subTest(file=rel):
                text = (ROOT / rel).read_text(encoding="utf-8")
                self.assertTrue(text.startswith("/* SPDX-License-Identifier: GPL-3.0-or-later */\n"))
                self.assertIn("Derived from the AMD FidelityFX SDK v2.4.0 (commit "
                              "7539ae8b6568ad29ed78a753404351daf3b5cca7)", text)
                self.assertIn("Copyright (C) 2026 Advanced Micro Devices, Inc.", text)
                self.assertIn("Permission is hereby granted, free of charge, to any person obtaining a copy", text)
                self.assertIn("THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND", text)
                self.assertRegex(text, r"Modified by Nakagawa Recomp contributors, 2026-\d\d-\d\d: ")

    def test_project_files_carry_no_amd_notice(self) -> None:
        for rel in self.PROJECT:
            with self.subTest(file=rel):
                text = (ROOT / rel).read_text(encoding="utf-8")
                self.assertIn("SPDX-License-Identifier: GPL-3.0-or-later", text.splitlines()[0])
                self.assertFalse("Advanced Micro Devices" in text, "an AMD notice in a project-authored file")


class MutantTextTests(unittest.TestCase):
    def test_every_mutant_text_matches_once(self) -> None:
        rows = [(m[0], m[1], m[2]) for m in fsr3_mutants.MUTANTS]
        rows += [(m[0], m[1], m[2]) for m in fsr3_mutants.PASS_MUTANTS]
        rows += [(m[0], m[1], m[2]) for m in fsr3_mutants.PASS_RECORDED]
        for name, path, old in rows:
            with self.subTest(mutant=name):
                self.assertEqual((ROOT / path).read_text(encoding="utf-8").count(old), 1)

    def test_every_gpu_mutant_names_a_cell(self) -> None:
        text = (ROOT / fsr3_mutants.PASS_SELFTEST).read_text(encoding="utf-8")
        cells = set(re.findall(r'\{ "([A-Z]-[0-9A-Z]+)", cell_', text))
        self.assertEqual(cells, {"P-0", "F-S", "F-M", "F-R", "F-T", "F-C", "F-D"})
        for mutant in fsr3_mutants.PASS_MUTANTS + fsr3_mutants.PASS_RECORDED:
            self.assertIn(mutant[4], cells, mutant[0])


class InertTests(unittest.TestCase):
    """F-A-lite's static half: PR-A adds no FSR 3 code to what the player builds or runs."""

    def test_no_fsr3_file_in_the_runtime_or_player_sources(self) -> None:
        for mk in ("mk/runtime_sources.mk", "mk/pipeline.mk"):
            with self.subTest(file=mk):
                self.assertNotIn("fsr3", (ROOT / mk).read_text(encoding="utf-8"))

    def test_no_runtime_or_player_source_refers_to_fsr3(self) -> None:
        own = {"fsr3_host.c", "fsr3_host.h", "fsr3_jitter.c", "fsr3_jitter.h", "fsr3_pass.c", "fsr3_pass.h",
               "fsr3_pass_selftest.c"}
        for path in sorted((ROOT / "src").rglob("*")):
            if path.suffix not in {".c", ".h", ".cpp", ".inc"} or path.name in own:
                continue
            if path.name.startswith("fsr3_") and path.suffix == ".inc":
                continue
            with self.subTest(file=path.relative_to(ROOT).as_posix()):
                self.assertNotRegex(path.read_text(encoding="utf-8", errors="replace"), r"fsr3_|FSR3_")


if __name__ == "__main__":
    unittest.main()
