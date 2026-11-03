# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Code a game installs at run time: captures, and their translation (#1933).

A run stopped by a call into code no translation covers writes a capture of the RAM around the
target (``SR_CODE_CAPTURE``). These tests pin what the build makes of one, on synthetic captures:

* the file format round-trips, and a malformed capture is refused by name;
* captures that overlap and agree make one image; captures whose bytes differ stay apart;
* an image converts to an ELF type 0xFFA8 overlay at its own address;
* the analysis is seeded only from the called targets, so a word that merely looks like code
  in the window is never translated;
* the translation's identity is the words it translated: data beside the code does not change
  it, a different code word does, so a capture whose bytes differ is never bound;
* ``codegen.py --code-capture`` emits and registers the descriptor, and refuses a bad capture.

The end-to-end proof that the runtime writes the capture and binds the translation only over the
same bytes is the native ``code-capture-selftest``.
"""

from __future__ import annotations

import contextlib
import io
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import analyze as analyzer
import code_capture
import codegen
from analyze import Elf, analyze, exec_ranges, in_ranges
from test_codegen_runtime_modules import _elf, _text

LOAD = 0x09C1B000
TARGET = LOAD + 0x780
LEAF = TARGET + 0x20
DATA = TARGET + 0x100            # a prologue-shaped word the game keeps beside its code
WINDOW = 0x1000
CALLER_PC, RA = 0x08804010, 0x08804018

ROUTINE = {
    TARGET + 0x00: 0x27BDFFF0,                              # addiu sp, sp, -16
    TARGET + 0x04: 0xAFBF0000,                              # sw    ra, 0(sp)
    TARGET + 0x08: 0x0C000000 | ((LEAF >> 2) & 0x03FFFFFF),  # jal   leaf
    TARGET + 0x0C: 0x24040005,                              # addiu a0, zero, 5
    TARGET + 0x10: 0x8FBF0000,                              # lw    ra, 0(sp)
    TARGET + 0x14: 0x03E00008,                              # jr    ra
    TARGET + 0x18: 0x27BD0010,                              # addiu sp, sp, 16
    LEAF + 0x00: 0x24820030,                                # addiu v0, a0, 0x30
    LEAF + 0x04: 0x03E00008,                                # jr    ra
    LEAF + 0x08: 0x00000000,                                # nop
}
CODE_END = LEAF + 0x0C


def window(edits=None, load=LOAD, size=WINDOW):
    data = bytearray(size)
    words = dict(ROUTINE)
    words[DATA] = 0x27BDFFE0                                # addiu sp, sp, -32, as data
    words.update(edits or {})
    for address, word in words.items():
        if load <= address < load + size:
            struct.pack_into("<I", data, address - load, word)
    return bytes(data)


def capture(edits=None, target=TARGET, load=LOAD, size=WINDOW, source="synthetic"):
    return code_capture.Capture(load, target, CALLER_PC, RA, window(edits, load, size), source)


def translate(image, index=0):
    with contextlib.redirect_stderr(io.StringIO()) as err:
        module = codegen.translate_code_capture(index, image)
    return module, err.getvalue()


def resident_digest(module, memory, load=LOAD):
    """The digest the runtime computes over ``memory`` at the module's check ranges."""
    words = b"".join(memory[lo - load:hi - load] for lo, hi in module.overlay_check_spans)
    return len(words), codegen.fnv1a64(words)


class CaptureFile(unittest.TestCase):
    def test_a_capture_round_trips(self):
        original = capture()
        blob = code_capture.capture_bytes(original)
        self.assertEqual(blob[:8], b"NKCODE01")
        self.assertEqual(struct.unpack_from("<6I", blob, 8),
                         (LOAD, WINDOW, TARGET, CALLER_PC, RA, 0))
        self.assertEqual(code_capture.parse_capture(blob, "synthetic"), original)
        self.assertEqual(code_capture.capture_file_name(original),
                         f"code_capture_{TARGET:08x}_{codegen.fnv1a64(original.data):016x}.nkcap")

    def test_the_digest_matches_the_runtime_vectors(self):
        # The runtime names a capture with sr_module_code_fnv1a64; codegen identifies code
        # with fnv1a64. The two must agree (FNV-1a 64-bit test vectors).
        for data, digest in ((b"", 0xCBF29CE484222325), (b"a", 0xAF63DC4C8601EC8C),
                             (b"foobar", 0x85944171F73967E8)):
            self.assertEqual(code_capture.fnv1a64(data), digest)
            self.assertEqual(codegen.fnv1a64(data), digest)

    def test_a_malformed_capture_is_refused_by_name(self):
        good = bytearray(code_capture.capture_bytes(capture()))
        cases = {
            "short": bytes(good[:20]),
            "magic": b"NKCODE02" + bytes(good[8:]),
            "reserved": bytes(good[:28]) + struct.pack("<I", 1) + bytes(good[32:]),
            "size": bytes(good[:-4]),
            "unaligned target": bytes(good[:16]) + struct.pack("<I", TARGET + 2) + bytes(good[20:]),
            "target outside": bytes(good[:16]) + struct.pack("<I", LOAD + WINDOW) + bytes(good[20:]),
            "empty": code_capture.HEADER.pack(code_capture.MAGIC, LOAD, 0, LOAD, 0, 0, 0),
        }
        for label, blob in cases.items():
            with self.subTest(label), self.assertRaises(code_capture.CaptureError) as caught:
                code_capture.parse_capture(blob, "bad.nkcap")
            self.assertIn("bad.nkcap", str(caught.exception))

    def test_a_folder_stands_for_its_captures_in_name_order(self):
        folder = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, folder, True)
        for name in ("b.nkcap", "a.NKCAP", "notes.txt"):
            (folder / name).write_bytes(code_capture.capture_bytes(capture()))
        self.assertEqual([p.name for p in code_capture.capture_paths([folder])],
                         ["a.NKCAP", "b.nkcap"])
        with self.assertRaises(code_capture.CaptureError):
            code_capture.capture_paths([folder / "missing.nkcap"])


class CaptureImages(unittest.TestCase):
    def test_overlapping_captures_that_agree_make_one_image(self):
        low = capture(load=LOAD, size=0x800, target=TARGET)
        high = capture(load=LOAD + 0x400, size=0x800, target=LEAF)
        images = code_capture.group_captures([high, low])
        self.assertEqual(len(images), 1)
        image = images[0]
        self.assertEqual((image.load, image.end), (LOAD, LOAD + 0xC00))
        self.assertEqual(image.targets, {TARGET, LEAF})
        self.assertEqual(bytes(image.data), window(size=0xC00))

    def test_captures_whose_bytes_differ_stay_apart(self):
        same = capture()
        other = capture({LEAF: 0x24820031})
        images = code_capture.group_captures([same, other, capture(source="again")])
        self.assertEqual(len(images), 2)
        self.assertEqual(sorted(len(image.sources) for image in images), [1, 2])
        self.assertEqual(len({image.name for image in images}), 2, "each image has its own name")

    def test_disjoint_captures_stay_apart(self):
        images = code_capture.group_captures([
            capture(load=LOAD, size=0x100, target=LOAD),
            capture(load=LOAD + 0x100, size=0x100, target=LOAD + 0x100)])
        self.assertEqual(len(images), 2)

    def test_an_image_is_a_fixed_address_overlay_at_its_own_address(self):
        image = code_capture.group_captures([capture()])[0]
        elf = code_capture.image_overlay_elf(image)
        self.assertTrue(analyzer.is_fixed_overlay(elf))
        self.assertEqual(analyzer.fixed_overlay_segment(elf), (LOAD, 84, WINDOW, WINDOW))
        self.assertEqual(elf[84:], bytes(image.data))


class CaptureTranslation(unittest.TestCase):
    def test_only_entries_seed_an_image_of_captured_memory(self):
        # The window's prologue-shaped data word seeds an ordinary fixed overlay's analysis;
        # with the captured target as the only seed, only the routine's code is traced.
        image = code_capture.group_captures([capture()])[0]
        elf = Elf(analyzer.code_overlay_elf(code_capture.image_overlay_elf(image)))
        ranges = exec_ranges(elf)
        plain, _ = analyze(elf, mixed_code_data=True)
        seeded, owned = analyze(elf, mixed_code_data=True, entries={TARGET})
        self.assertIn(DATA, {a for a in plain if in_ranges(a, ranges)})
        self.assertEqual({a for a in seeded if in_ranges(a, ranges)}, {TARGET, LEAF})

    def test_the_translation_is_the_code_its_targets_reach(self):
        module, err = translate(code_capture.group_captures([capture()])[0])
        self.assertIsNone(module.unsupported_reason, err)
        self.assertEqual(dict(module.functions), {TARGET: 7, LEAF: 3})
        self.assertEqual(module.spans, [(TARGET, TARGET + 0x1C), (LEAF, CODE_END)])
        self.assertEqual(module.overlay_check_spans, ((TARGET, TARGET + 0x1C), (LEAF, CODE_END)))
        self.assertEqual((module.link_low, module.link_high, module.relocatable),
                         (TARGET, CODE_END, False), "the range is the code, not the window")
        self.assertNotIn("has no translation", err)
        descriptor = "\n".join(codegen.runtime_module_descriptor_lines(module))
        self.assertIn(f"0x{TARGET:08x}u, 0x{CODE_END:08x}u, 0x4u, 0u,", descriptor)
        self.assertIn(f"0x{module.overlay_check_len:08x}u, 0x{module.overlay_check_fnv1a64:016x}ull",
                      descriptor)

    def test_a_capture_whose_bytes_differ_is_not_bound(self):
        # The runtime binds the translation only while memory digests to its identity
        # (overlay_spans_resident). Data beside the code is not part of it; code is.
        module, _ = translate(code_capture.group_captures([capture()])[0])
        own = (module.overlay_check_len, module.overlay_check_fnv1a64)
        self.assertEqual(resident_digest(module, window()), own)
        self.assertEqual(resident_digest(module, window({DATA: 0x13572468, DATA + 4: 7})), own,
                         "data beside the code does not change the identity")
        for address in (TARGET, TARGET + 0x0C, LEAF):
            changed = window({address: ROUTINE[address] ^ 1})
            self.assertNotEqual(resident_digest(module, changed), own,
                                f"a different word at 0x{address:08x} is not the same code")
        other, _ = translate(code_capture.group_captures([capture({LEAF: 0x24820031})])[0], 1)
        self.assertNotEqual((other.overlay_check_len, other.overlay_check_fnv1a64), own)
        self.assertNotEqual(other.name, module.name)

    def test_a_target_without_a_translation_is_named(self):
        # A target on a word the emitter refuses (COP3) gets no body; the build says so. Its
        # trace runs on through the zeros after it, and none of that is identity, so none of
        # it is executable authority or inside the bound range: a call there stops as
        # target-not-executable and is captured again.
        dead = TARGET + 0x200
        images = code_capture.group_captures(
            [capture({dead: 0x4C000000}, target=dead), capture({dead: 0x4C000000})])
        self.assertEqual(len(images), 1)
        module, err = translate(images[0])
        self.assertIn(f"captured target 0x{dead:08x} has no translation", err)
        self.assertEqual([a for a, _ in module.stubbed], [dead])
        self.assertEqual(dict(module.functions), {TARGET: 7, LEAF: 3})
        self.assertEqual(len(module.texts), 2)
        self.assertEqual(module.spans, list(module.overlay_check_spans))
        self.assertEqual(module.spans, [(TARGET, TARGET + 0x1C), (LEAF, CODE_END)])
        self.assertEqual((module.link_low, module.link_high), (TARGET, CODE_END))
        for word in (dead, dead + 4, dead + 0x100, LOAD + WINDOW - 4):
            self.assertFalse(in_ranges(word, module.spans), f"0x{word:08x} has no authority")

    def test_executable_authority_is_exactly_the_identity(self):
        # Every word the interpreter may run inside a bound capture is a word the runtime
        # checked against the capture before binding it.
        for edits, targets in (({}, {TARGET}), ({TARGET + 0x200: 0x4C000000},
                                                {TARGET, TARGET + 0x200})):
            image = code_capture.group_captures(
                [capture(edits, target=target) for target in sorted(targets)])[0]
            module, _ = translate(image)
            with self.subTest(targets=sorted(targets)):
                self.assertEqual(module.spans, list(module.overlay_check_spans))
                self.assertLessEqual(module.link_low, module.spans[0][0])
                self.assertEqual(module.link_high, module.spans[-1][1])

    def test_an_image_with_no_code_gets_a_named_descriptor(self):
        dead = TARGET + 0x200
        module, err = translate(code_capture.group_captures(
            [capture({dead: 0x4C000000}, target=dead)])[0])
        self.assertEqual(module.unsupported_reason,
                         "code overlay has no translated code to identify it")
        self.assertEqual(module.overlay_check_spans, ())
        self.assertIn(f"captured target 0x{dead:08x} has no translation", err)

    def test_the_unit_registers_every_image(self):
        images = code_capture.group_captures([capture(), capture({LEAF: 0x24820031})])
        with contextlib.redirect_stderr(io.StringIO()):
            modules = code_capture.translate_images(images)
        text = code_capture.unit_text(modules)
        self.assertIn("void sr_register_code_captures(void) {\n"
                      "    sr_module_code_register(&sr_m0_code);\n"
                      "    sr_module_code_register(&sr_m1_code);\n}", text)
        for module in modules:
            self.assertIn(f"void m{module.index}_{TARGET:08x}(CpuState *s) {{", text)


class CodegenCodeCaptureOption(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root, True)
        self.elf = self.root / "main.elf"
        self.elf.write_bytes(_elf(_text()))

    def run_codegen(self, *options):
        out_c = self.root / "game_recomp.c"
        with contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()) as err:
            status = codegen.main(["codegen.py", str(self.elf), str(out_c), *options])
        return status, err.getvalue(), out_c

    def test_codegen_translates_and_registers_a_capture_folder(self):
        folder = self.root / "captures"
        folder.mkdir()
        (folder / "one.nkcap").write_bytes(code_capture.capture_bytes(capture()))
        status, err, out_c = self.run_codegen(f"--code-capture={folder}")
        self.assertEqual(status, 0, err)
        main_c = out_c.read_text(encoding="ascii")
        name = code_capture.group_captures([capture()])[0].name
        self.assertIn(f'"{name}"', main_c)
        self.assertIn("    sr_module_code_register(&sr_m0_code);", main_c)
        chunks = "".join(p.read_text(encoding="ascii")
                         for p in self.root.glob("game_recomp_*.c"))
        self.assertIn(f"void m0_{TARGET:08x}(CpuState *s) {{", chunks)
        self.assertNotIn(f"sr_register(0x{TARGET:08x}u", chunks,
                         "a capture is reached through its descriptor only")

    def test_without_captures_the_output_is_unchanged(self):
        status, err, out_c = self.run_codegen()
        self.assertEqual(status, 0, err)
        self.assertNotIn("code_capture_", out_c.read_text(encoding="ascii"))

    def test_codegen_refuses_a_malformed_capture(self):
        bad = self.root / "bad.nkcap"
        bad.write_bytes(b"NKCODE01" + bytes(4))
        status, err, _ = self.run_codegen(f"--code-capture={bad}")
        self.assertEqual(status, 2)
        self.assertIn("codegen: bad.nkcap: shorter than the 32-byte header", err)


if __name__ == "__main__":
    unittest.main()
