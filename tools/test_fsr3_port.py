# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""FSR 3's C port checked against the files it depends on.

* fsr3_pass_layout (src/rt/fsr3_host.c) against each embedded module's own decorations: every
  descriptor's binding, kind and name, which SRVs are sampled through s_LinearClamp, the local size,
  and the constant buffers' member order and offsets against the C structs in fsr3_host.h.
* fsr3_fg_pass_layout (src/rt/fsr3_fg_host.c), row by row against the frame-generation manifest, the
  same way: binding, kind (storage buffers read-only or writable), name, linear sampling, local size,
  the optical-flow and subgroup flags, each constant buffer row's stage and its C struct.
* The storage images that declare a format are exactly the atomic ones, as R32ui (the LOCAL DELTA),
  in the upscaler set and, image by image across passes, in the frame-generation set.
* Only frame generation's optical-flow search uses subgroup operations, the one pass that needs a
  required subgroup size of 32.
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
OP_SAMPLED_IMAGE, OP_DECORATE, OP_MEMBER_DECORATE, OP_CAPABILITY = 86, 71, 72, 17
CAP_GROUP_NON_UNIFORM = 61
DEC_BINDING, DEC_DESCRIPTOR_SET, DEC_OFFSET, DEC_BUFFER_BLOCK, DEC_NON_WRITABLE = 33, 34, 35, 3, 24
MODE_LOCAL_SIZE, FORMAT_UNKNOWN, FORMAT_R32UI = 17, 0, 33
OP_TYPE_VECTOR, OP_IMAGE_WRITE = 23, 99
# Instructions with operands but no result type: everything else that has a result id has its type in
# its first operand. OpType* (19-39) have a result id only.
NO_RESULT_TYPE = {2, 3, 4, 5, 6, 7, 8, 10, 11, 14, 15, 16, 17, 62, 63, 71, 72, 73, 74, 75, 99, 224, 225, 228,
                  246, 247, 249, 250, 251, 254, 317, 330, 332, 5632, 5633}


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
    buffer_blocks, read_only = set(), set()
    vectors, value_type, image_writes = {}, {}, []
    local = None
    caps = set()
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xFFFF
        a = words[i + 1:i + count]
        if op == OP_CAPABILITY:
            caps.add(a[0])
        elif op == OP_NAME:
            names[a[0]] = _string(a[1:])
        elif op == OP_MEMBER_NAME:
            member_names[(a[0], a[1])] = _string(a[2:])
        elif op == OP_DECORATE and a[1] == DEC_BINDING:
            binding[a[0]] = a[2]
        elif op == OP_DECORATE and a[1] == DEC_DESCRIPTOR_SET:
            dset[a[0]] = a[2]
        elif op == OP_DECORATE and a[1] == DEC_BUFFER_BLOCK:
            buffer_blocks.add(a[0])
        elif op == OP_MEMBER_DECORATE and a[2] == DEC_OFFSET:
            offsets[(a[0], a[1])] = a[3]
        elif op == OP_MEMBER_DECORATE and a[2] == DEC_NON_WRITABLE:
            read_only.add(a[0])
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
        elif op == OP_TYPE_VECTOR:
            vectors[a[0]] = a[2]
        elif op == OP_IMAGE_WRITE:
            image_writes.append((a[0], a[2]))
        if op not in NO_RESULT_TYPE and not 19 <= op <= 39 and count >= 3:
            value_type[a[1]] = a[0]
        i += count
    # The components of each storage image's writes, by the image's name.
    writes = {}
    for image, texel in image_writes:
        name = names.get(loads.get(image), "")
        writes.setdefault(name, set()).add(vectors.get(value_type.get(texel), 1))
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
        elif pointee in buffer_blocks:
            # A structured buffer (the Uniform class with BufferBlock, as DXC emits it), read-only
            # when its member is NonWritable.
            what = "SRV_BUFFER" if pointee in read_only else "UAV_BUFFER"
        else:
            what = "CBV"
            cbuffers[name] = [(member_names.get((pointee, m)), offsets.get((pointee, m))) for m in range(kind[1])]
        linear = any(loads.get(image) == var for image in sampled)
        rows.append((number, dset.get(var), what, name, linear))
    return {"local": local, "rows": sorted(rows), "formats": formats, "cbuffers": cbuffers, "caps": caps,
            "writes": writes}


def c_tables(path: Path = HOST_C, struct: str = "Fsr3Binding",
             prefix: str = "FSR3_BIND_") -> dict[str, list[tuple[int, str, str, bool]]]:
    return {name: [(b, kind, n, linear) for b, kind, _id, linear, n in rows]
            for name, rows in c_table_rows(path, struct, prefix).items()}


def c_table_rows(path: Path, struct: str, prefix: str) -> dict[str, list[tuple[int, str, str, bool, str]]]:
    """Each binding table's rows: (binding, kind, id text, linear, name)."""
    text = path.read_text(encoding="utf-8")
    tables = {}
    for name, body in re.findall(rf"static const {struct} (\w+)\[\] = \{{\n(.*?)\n\}};", text, re.S):
        rows = re.findall(rf'\{{ (\d+), {prefix}(\w+), ([^,]+), ([01]), "(\w+)" \}}', body)
        if len(rows) != body.count("{"):
            raise ValueError(f"{name}: unreadable row")
        tables[name] = [(int(b), kind, ident, linear == "1", n) for b, kind, ident, linear, n in rows]
    return tables


def c_layouts() -> list[tuple[str, int, int, str]]:
    text = HOST_C.read_text(encoding="utf-8")
    return [(label, int(x), int(y), table)
            for label, x, y, table in re.findall(r'LAYOUT\("([\w-]+)", (\d+), (\d+), (\w+)\)', text)]


def c_struct_fields(name: str, header: Path = HOST_H) -> list[tuple[str, int]]:
    """(field, bytes) of a typedef'd struct of 4-byte scalars and arrays in a header."""
    text = header.read_text(encoding="utf-8")
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


FG_HOST_C = ROOT / "src/rt/fsr3_fg_host.c"
FG_HOST_H = ROOT / "src/rt/fsr3_fg_host.h"
# Frame generation's constant buffers: the stage the CBV row reads and the C struct it is.
FG_CBUFFER = {"cbFI": ("CB_FI", "Fsr3FgConstants"), "cbInpaintingPyramid": ("CB_INPAINTING", "Fsr3FgInpaintingConstants"),
              "cbOF": ("CB_OF", "Fsr3OfConstants")}
# HLSL member names that c_name does not turn into the C field's, which keeps AMD's C++ spelling
# (FrameInterpolationConstants, InpaintingPyramidConstants in ffx_frameinterpolation_private.h).
FG_HLSL_RENAMED = {"Mode": "Mode", "reset": "Reset", "HUDLessAttachedFactor": "HUDLessAttachedFactor",
                   "fTanHalfFOV": "fTanHalfFOV", "fJitter": "jitter", "numWorkGroups": "numworkGroups"}


def fg_c_name(hlsl: str) -> str:
    return FG_HLSL_RENAMED.get(hlsl) or c_name(hlsl)


def c_fg_layouts() -> list[tuple[str, tuple[int, int, int], bool, bool, str]]:
    text = FG_HOST_C.read_text(encoding="utf-8")
    return [(label, (int(x), int(y), int(z)), of == "1", s32 == "1", table)
            for label, x, y, z, of, s32, table in
            re.findall(r'LAYOUT\("(\w+)", (\d+), (\d+), (\d+), ([01]), ([01]), (\w+)\)', text)]


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


class FrameGenerationShaderTests(unittest.TestCase):
    """The fsr3fg set: AMD's frame-interpolation and optical-flow passes (PR-C)."""

    def setUp(self) -> None:
        self.modules = {Path(emb).name: reflect(spirv_words(ROOT / emb))
                        for _, _, emb in shader_embed.FSR3FG_SHADERS}

    def test_every_pass_is_embedded_once(self) -> None:
        self.assertEqual(len(self.modules), 17)
        self.assertEqual(set(self.modules), {p.name for p in GPU_DIR.glob("fsr3fg_*_comp.inc")})

    def test_images_declare_r32ui_exactly_when_an_atomic_reaches_them_in_some_pass(self) -> None:
        declared, atomic = {}, set()
        for module, facts in self.modules.items():
            for name, (image_format, is_atomic) in facts["formats"].items():
                self.assertIn(image_format, (FORMAT_UNKNOWN, FORMAT_R32UI), (module, name))
                declared.setdefault(name, set()).add(image_format)
                if is_atomic:
                    atomic.add(name)
        for name, formats in declared.items():
            with self.subTest(image=name):
                self.assertEqual(formats, {FORMAT_R32UI} if name in atomic else {FORMAT_UNKNOWN})
        self.assertEqual(atomic, {
            "rw_reconstructed_depth_previous_frame", "rw_reconstructed_depth_interpolated_frame",
            "rw_game_motion_vector_field_x", "rw_game_motion_vector_field_y",
            "rw_optical_flow_motion_vector_field_x", "rw_optical_flow_motion_vector_field_y",
            "rw_optical_flow_scd_histogram", "rw_optical_flow_scd_temp", "rw_optical_flow_scd_output"})

    def test_storage_writes_have_the_components_of_their_images(self) -> None:
        # Vulkan requires a write's texel to have at least as many components as the image's format
        # (VUID-vkCmdDispatch-OpImageWrite-08795): the four-component images (the output, in the colour
        # format, and the inpainting pyramid) take four, and no pass writes three.
        four = {"rw_output"} | {f"rw_inpainting_pyramid{m}" for m in range(13)}
        seen = set()
        for module, facts in self.modules.items():
            for name, counts in facts["writes"].items():
                with self.subTest(module=module, image=name):
                    self.assertTrue(name, "a write to an image that is not a named variable")
                    self.assertNotIn(3, counts)
                    if name in four:
                        self.assertEqual(counts, {4})
                        seen.add(name)
        self.assertIn("rw_output", seen)
        self.assertIn("rw_inpainting_pyramid0", seen)

    def test_only_the_optical_flow_search_uses_subgroup_operations(self) -> None:
        users = {m for m, facts in self.modules.items() if CAP_GROUP_NON_UNIFORM in facts["caps"]}
        self.assertEqual(users, {"fsr3fg_of_compute_optical_flow_advanced_v5_comp.inc"})

    def test_layouts_follow_the_manifest_and_match_the_spirv(self) -> None:
        """fsr3_fg_pass_layout (src/rt/fsr3_fg_host.c), row n against manifest entry n: every
        descriptor's binding, kind (storage buffers read-only or not), name and linear sampling, the
        local size, the optical-flow flag and the subgroup flag."""
        tables = c_tables(FG_HOST_C, "Fsr3FgBinding", "FSR3FG_BIND_")
        layouts = c_fg_layouts()
        self.assertEqual(len(layouts), len(shader_embed.FSR3FG_SHADERS))
        for (label, local, of, s32, table), (_, _, emb) in zip(layouts, shader_embed.FSR3FG_SHADERS, strict=True):
            name = Path(emb).name
            with self.subTest(label=label, module=name):
                facts = self.modules[name]
                self.assertTrue(all(s == 0 for _, s, *_ in facts["rows"]), "every descriptor is in set 0")
                self.assertEqual(tables[table], [(b, kind, n, linear) for b, _s, kind, n, linear in facts["rows"]])
                self.assertEqual(facts["local"], local)
                self.assertEqual(of, name.startswith("fsr3fg_of_"))
                self.assertEqual(s32, CAP_GROUP_NON_UNIFORM in facts["caps"])

    def test_constant_buffer_rows_name_their_stage_and_match_the_c_structs(self) -> None:
        rows = c_table_rows(FG_HOST_C, "Fsr3FgBinding", "FSR3FG_BIND_")
        layouts = c_fg_layouts()
        seen = set()
        for (label, *_, table), (_, _, emb) in zip(layouts, shader_embed.FSR3FG_SHADERS, strict=True):
            cbuffers = self.modules[Path(emb).name]["cbuffers"]
            for binding, kind, ident, _linear, cb in rows[table]:
                if kind != "CBV":
                    continue
                with self.subTest(label=label, cbuffer=cb):
                    self.assertEqual(ident, FG_CBUFFER[cb][0])
                    fields = c_struct_fields(FG_CBUFFER[cb][1], FG_HOST_H)
                    members = cbuffers[cb]
                    self.assertEqual([fg_c_name(m) for m, _ in members], [f for f, _ in fields])
                    offset = 0
                    for (member, spirv_offset), (_, size) in zip(members, fields, strict=True):
                        self.assertEqual(spirv_offset, offset, member)
                        offset += size
                    seen.add(cb)
        # cbOF_SPD is staged but no compiled pass declares it (fsr3_fg_host.h).
        self.assertEqual(seen, set(FG_CBUFFER))

    def test_the_manifest_pins_the_shader_options(self) -> None:
        for define in ("FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH=1",
                       "FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=1",
                       "FFX_FRAMEINTERPOLATION_OPTION_JITTERED_MOTION_VECTORS=0",
                       "FFX_SPD_NO_WAVE_OPERATIONS=1", "FFX_HALF=0"):
            self.assertIn(define, shader_embed.FSR3FG_DEFINES)
        self.assertFalse(any("PREFER_WAVE64" in d for d in shader_embed.FSR3FG_DEFINES))


class NoticeTests(unittest.TestCase):
    DERIVED = ("src/rt/fsr3_host.c", "src/rt/fsr3_host.h", "src/rt/fsr3_jitter.c", "src/rt/fsr3_jitter.h",
               "src/rt/gpu_sdl3vk/fsr3_pass.c", "src/rt/fsr3_fg_host.c", "src/rt/fsr3_fg_host.h",
               "src/rt/gpu_sdl3vk/fsr3_fg_pass.c")
    PROJECT = ("src/rt/gpu_sdl3vk/fsr3_pass.h", "src/rt/gpu_sdl3vk/fsr3_pass_selftest.c",
               "tests/native/fsr3_host_selftest.c", "tests/native/fsr3_fg_host_selftest.c", "tools/fsr3_mutants.py",
               "src/rt/fsr3_fg_state.c", "src/rt/fsr3_fg_state.h", "tests/native/fsr3_fg_state_selftest.c",
               "src/rt/gpu_sdl3vk/fsr3_fg_pass.h", "src/rt/gpu_sdl3vk/fsr3_fg_pass_selftest.c")

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
        rows += [(m[0], m[1], m[2]) for m in fsr3_mutants.FG_MUTANTS]
        rows += [(m[0], m[1], m[2]) for m in fsr3_mutants.FG_PASS_MUTANTS]
        rows += [(m[0], m[1], m[2]) for m in fsr3_mutants.FG_PASS_RECORDED]
        for name, path, old in rows:
            with self.subTest(mutant=name):
                self.assertEqual((ROOT / path).read_text(encoding="utf-8").count(old), 1)

    def test_every_gpu_mutant_names_a_cell(self) -> None:
        text = (ROOT / fsr3_mutants.PASS_SELFTEST).read_text(encoding="utf-8")
        cells = set(re.findall(r'\{ "([A-Z]-[0-9A-Z]+)", cell_', text))
        self.assertEqual(cells, {"P-0", "F-S", "F-M", "F-R", "F-T", "F-C", "F-D"})
        for mutant in fsr3_mutants.PASS_MUTANTS + fsr3_mutants.PASS_RECORDED:
            self.assertIn(mutant[4], cells, mutant[0])

    def test_every_frame_generation_gpu_mutant_names_a_cell(self) -> None:
        text = (ROOT / fsr3_mutants.FG_PASS_SELFTEST).read_text(encoding="utf-8")
        cells = set(re.findall(r'\{ "([A-Z]-[0-9A-Z]+)", cell_', text))
        self.assertEqual(cells, {"G-R", "G-0", "G-M", "G-D", "G-H", "G-C"})
        for mutant in fsr3_mutants.FG_PASS_MUTANTS + fsr3_mutants.FG_PASS_RECORDED:
            self.assertIn(mutant[4], cells, mutant[0])
        # Every quality cell has a mutant: a cell no defect can fail proves nothing.
        self.assertEqual({m[4] for m in fsr3_mutants.FG_PASS_MUTANTS}, cells - {"G-R"})


class InertTests(unittest.TestCase):
    """F-A-lite's static half: no FSR 3 code is in what the player builds or runs."""

    def test_no_fsr3_file_in_the_runtime_or_player_sources(self) -> None:
        for mk in ("mk/runtime_sources.mk", "mk/pipeline.mk"):
            with self.subTest(file=mk):
                self.assertNotIn("fsr3", (ROOT / mk).read_text(encoding="utf-8"))

    def test_no_runtime_or_player_source_refers_to_fsr3(self) -> None:
        own = {"fsr3_host.c", "fsr3_host.h", "fsr3_jitter.c", "fsr3_jitter.h", "fsr3_pass.c", "fsr3_pass.h",
               "fsr3_pass_selftest.c", "fsr3_fg_host.c", "fsr3_fg_host.h", "fsr3_fg_state.c", "fsr3_fg_state.h",
               "fsr3_fg_pass.c", "fsr3_fg_pass.h", "fsr3_fg_pass_selftest.c"}
        for path in sorted((ROOT / "src").rglob("*")):
            if path.suffix not in {".c", ".h", ".cpp", ".inc"} or path.name in own:
                continue
            if path.name.startswith("fsr3_") and path.suffix == ".inc":
                continue
            with self.subTest(file=path.relative_to(ROOT).as_posix()):
                self.assertNotRegex(path.read_text(encoding="utf-8", errors="replace"), r"fsr3_|FSR3_")


if __name__ == "__main__":
    unittest.main()
