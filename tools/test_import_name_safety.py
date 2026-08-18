# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Synthetic regressions for untrusted PSP import-library metadata."""

from __future__ import annotations

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import tomllib  # noqa: E402
import imports as imports_tool  # noqa: E402


CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


def build_synthetic_import_prx(name: bytes, base: int) -> tuple[bytes, int]:
    """Build a minimal ET_SCE_PRX with one real import stub and no relocations."""
    data_off = 0x100
    text_off = 0x00
    module_off = 0x40
    stub_off = 0x80
    name_off = 0x90
    nid_off = (name_off + len(name) + 1 + 3) & ~3
    entry_off = nid_off + 4
    segment = bytearray(entry_off + 20)

    # jr $ra; nop -- enough executable input for the real analyzer/codegen path.
    struct.pack_into("<2I", segment, text_off, 0x03E00008, 0)
    struct.pack_into("<2I", segment, stub_off, 0x03E00008, 0)
    segment[name_off:name_off + len(name)] = name
    segment[name_off + len(name)] = 0
    struct.pack_into("<I", segment, nid_off, 0x12345678)
    struct.pack_into(
        "<IHHBBHII",
        segment,
        entry_off,
        base + name_off,
        0x0101,
        0x0009,
        5,
        0,
        1,
        base + nid_off,
        base + stub_off,
    )
    struct.pack_into(
        "<2I", segment, module_off + 44, base + entry_off, base + entry_off + 20
    )

    names = b"\0.text\0.rodata.sceModuleInfo\0.sceStub.text\0.lib.stub\0.shstrtab\0"
    name_offsets = {
        part: names.index(part.encode("ascii"))
        for part in (
            ".text",
            ".rodata.sceModuleInfo",
            ".sceStub.text",
            ".lib.stub",
            ".shstrtab",
        )
    }
    shstr_off = data_off + len(segment)
    shoff = (shstr_off + len(names) + 3) & ~3
    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    header = ident + struct.pack(
        "<HHIIIIIHHHHHH",
        0xFFA0,
        8,
        1,
        text_off,
        52,
        shoff,
        0,
        52,
        32,
        1,
        40,
        6,
        5,
    )
    phdr = struct.pack(
        "<8I", 1, data_off, 0, 0, len(segment), len(segment), 7, 0x1000
    )
    prefix = header + phdr + b"\0" * (data_off - len(header) - len(phdr))
    padding = b"\0" * (shoff - shstr_off - len(names))

    def section(name: str, flags: int, addr: int, off: int, size: int, align: int):
        return struct.pack(
            "<10I", name_offsets[name], 1, flags, addr, off, size, 0, 0, align, 0
        )

    sections = [struct.pack("<10I", *([0] * 10))]
    sections.append(section(".text", 6, text_off, data_off + text_off, 8, 4))
    sections.append(
        section(
            ".rodata.sceModuleInfo", 2, module_off, data_off + module_off, 52, 4
        )
    )
    sections.append(
        section(".sceStub.text", 6, stub_off, data_off + stub_off, 8, 4)
    )
    sections.append(
        section(".lib.stub", 2, entry_off, data_off + entry_off, 20, 4)
    )
    sections.append(
        struct.pack(
            "<10I", name_offsets[".shstrtab"], 3, 0, 0, shstr_off, len(names), 0, 0, 1, 0
        )
    )
    return prefix + bytes(segment) + names + padding + b"".join(sections), base + stub_off


class FakeElf:
    """Small mapped guest image containing one PSP import stub."""

    BASE = 0x1000
    MODULE = 0x1000
    STUB = 0x1100
    NAME = 0x1200
    NIDS = 0x1300
    FIRST_SYM = 0x1400

    def __init__(self, name: bytes, *, terminate: bool = True, size: int = 0x1000):
        self.mem = bytearray(size)
        struct.pack_into("<2I", self.mem, self.MODULE - self.BASE + 44,
                         self.STUB, self.STUB + 28)
        struct.pack_into(
            "<IHHBBHII",
            self.mem,
            self.STUB - self.BASE,
            self.NAME,
            0,
            0,
            7,       # entry size in 32-bit words => 28 bytes
            0,       # numVars
            1,       # numFuncs
            self.NIDS,
            self.FIRST_SYM,
        )
        struct.pack_into("<I", self.mem, self.NIDS - self.BASE, 0x12345678)
        start = self.NAME - self.BASE
        end = start + len(name)
        if end > len(self.mem):
            raise ValueError("test fixture name does not fit")
        self.mem[start:end] = name
        if terminate:
            if end >= len(self.mem):
                raise ValueError("test fixture terminator does not fit")
            self.mem[end] = 0

    def sec(self, name: str):
        if name == ".rodata.sceModuleInfo":
            return {"addr": self.MODULE}
        return None

    def read_at_vaddr(self, addr: int, n: int):
        off = addr - self.BASE
        if off < 0 or n < 0 or off + n > len(self.mem):
            return None
        return bytes(self.mem[off:off + n])


class TestImportNameSafety(unittest.TestCase):
    def test_missing_module_info_section_is_valueerror_not_systemexit(self):
        # Found by tools/test_parse_fuzz.py: a PRX without .rodata.sceModuleInfo
        # used to raise SystemExit from the library path, killing any host
        # process that consumed parse_imports (analyze.py/codegen.py).
        class NoModuleInfoElf:
            base = 0

            def sec(self, name):  # noqa: ARG002
                return None

            def read_at_vaddr(self, addr, n):  # noqa: ARG002
                return None

        with self.assertRaisesRegex(ValueError, "no .rodata.sceModuleInfo"):
            imports_tool.parse_imports(NoModuleInfoElf())

    def test_normal_library_name_is_unchanged(self):
        parsed = imports_tool.parse_imports(FakeElf(b"sceDisplay"))
        self.assertEqual(parsed[FakeElf.FIRST_SYM], ("sceDisplay", 0x12345678))

    def test_comment_and_control_bytes_are_percent_encoded(self):
        hostile = b'evil*/\n"\\\xff'
        parsed = imports_tool.parse_imports(FakeElf(hostile))
        lib, nid = parsed[FakeElf.FIRST_SYM]
        self.assertEqual(nid, 0x12345678)
        self.assertEqual(lib, "evil%2A%2F%0A%22%5C%FF")
        self.assertNotIn("*/", lib)
        self.assertNotIn("\n", lib)
        self.assertNotIn('"', lib)
        self.assertTrue(lib.isascii())
        self.assertTrue(all(c.isalnum() or c in "_.-%" for c in lib))

    def test_percent_is_encoded_so_representation_is_reversible(self):
        self.assertEqual(imports_tool._encode_import_library_name(b"A%2FB"), "A%252FB")

    def test_toml_basic_string_never_emits_raw_syntax_from_encoded_name(self):
        encoded = imports_tool._encode_import_library_name(b'a"\n\\\xff')
        literal = imports_tool._toml_basic_string(encoded)
        self.assertEqual(literal, '"a%22%0A%5C%FF"')
        self.assertNotIn("\n", literal)

    def test_overlong_unterminated_library_name_is_rejected(self):
        limit = imports_tool.MAX_IMPORT_LIBRARY_NAME_BYTES
        elf = FakeElf(b"A" * (limit + 1), terminate=False, size=0x2000)
        with self.assertRaisesRegex(ValueError, "exceeds"):
            imports_tool.parse_imports(elf)

    def test_library_name_leaving_mapped_input_is_rejected(self):
        # Move the name pointer to the final byte and make it non-NUL so the next read is unmapped.
        elf = FakeElf(b"ok")
        last_addr = elf.BASE + len(elf.mem) - 1
        struct.pack_into("<I", elf.mem, elf.STUB - elf.BASE, last_addr)
        elf.mem[-1] = ord("X")
        with self.assertRaisesRegex(ValueError, "leaves mapped input"):
            imports_tool.parse_imports(elf)

    def test_truncated_module_info_is_rejected(self):
        elf = FakeElf(b"sceDisplay")
        original = elf.read_at_vaddr

        def truncated(addr: int, n: int):
            if addr == elf.MODULE and n == 52:
                return b"\0" * 12
            return original(addr, n)

        elf.read_at_vaddr = truncated
        with self.assertRaisesRegex(ValueError, "truncated .rodata.sceModuleInfo"):
            imports_tool.parse_imports(elf)

    def test_import_address_arithmetic_overflow_is_rejected(self):
        elf = FakeElf(b"sceDisplay")
        # Two functions make the second stub address exceed UINT32_MAX.
        off = elf.STUB - elf.BASE
        struct.pack_into("<H", elf.mem, off + 10, 2)  # numFuncs
        struct.pack_into("<I", elf.mem, off + 16, 0xFFFFFFFC)  # firstSym
        with self.assertRaisesRegex(ValueError, "address arithmetic"):
            imports_tool.parse_imports(elf)

    @unittest.skipUnless(CC, "no C compiler on PATH")
    def test_malicious_prx_cli_codegen_and_compiler_integration(self):
        assert CC is not None
        primary_base = 0x08804000
        extra_base = 0x08904000
        payload = b'evil*/\n#define IMPORT_NAME_INJECTED 1\r\n"\\%\x01\xff'
        encoded = imports_tool._encode_import_library_name(payload)

        with tempfile.TemporaryDirectory(prefix="import_name_integration_") as td:
            tmp = Path(td)
            hostile = tmp / "hostile.prx"
            ordinary = tmp / "ordinary.prx"
            hostile_blob, hostile_stub = build_synthetic_import_prx(
                payload, primary_base
            )
            ordinary_blob, _ = build_synthetic_import_prx(
                b"sceDisplay", primary_base
            )
            hostile.write_bytes(hostile_blob)
            ordinary.write_bytes(ordinary_blob)

            toml_path = tmp / "hostile.toml"
            imports_result = subprocess.run(
                [
                    sys.executable,
                    os.fspath(TOOLS / "imports.py"),
                    os.fspath(hostile),
                    f"{primary_base:x}",
                    f"--toml={toml_path}",
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                imports_result.returncode,
                0,
                imports_result.stdout + imports_result.stderr,
            )
            parsed_toml = tomllib.loads(toml_path.read_text(encoding="ascii"))
            self.assertEqual(
                parsed_toml["import"],
                [{"stub": hostile_stub, "lib": encoded, "nid": 0x12345678}],
            )

            def generate_and_compile(
                stem: str, main_prx: Path, extra_prx: Path | None = None
            ) -> str:
                out_c = tmp / f"{stem}.c"
                command = [
                    sys.executable,
                    os.fspath(TOOLS / "codegen.py"),
                    os.fspath(main_prx),
                    os.fspath(out_c),
                    f"--base={primary_base:x}",
                    "--funcs-per-chunk=2000",
                ]
                if extra_prx is not None:
                    command.append(f"--extra-elf={extra_prx}@{extra_base:x}")
                env = os.environ.copy()
                env["GAME_BASE"] = hex(primary_base)
                env["HST_EXTRA_SPANS"] = ""
                generated = subprocess.run(
                    command, cwd=ROOT, env=env, capture_output=True, text=True
                )
                self.assertEqual(
                    generated.returncode, 0, generated.stdout + generated.stderr
                )

                chunks = sorted(tmp.glob(f"{stem}_[0-9]*.c"))
                self.assertTrue(chunks, "codegen emitted no compilable chunk")
                combined = "\n".join(
                    chunk.read_text(encoding="ascii") for chunk in chunks
                )
                self.assertIn(f"import: {encoded} nid 0x12345678", combined)
                for chunk in chunks:
                    obj = chunk.with_suffix(".o")
                    build = subprocess.run(
                        [
                            CC,
                            "-std=c11",
                            "-Wall",
                            "-Wextra",
                            "-Werror",
                            "-Wno-unused-function",
                            f"-I{tmp}",
                            f"-I{ROOT / 'src' / 'rt'}",
                            "-c",
                            os.fspath(chunk),
                            "-o",
                            os.fspath(obj),
                        ],
                        cwd=ROOT,
                        capture_output=True,
                        text=True,
                    )
                    self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                    preprocessed = subprocess.run(
                        [
                            CC,
                            "-dM",
                            "-E",
                            f"-I{tmp}",
                            f"-I{ROOT / 'src' / 'rt'}",
                            os.fspath(chunk),
                        ],
                        cwd=ROOT,
                        capture_output=True,
                        text=True,
                    )
                    self.assertEqual(
                        preprocessed.returncode,
                        0,
                        preprocessed.stdout + preprocessed.stderr,
                    )
                    self.assertNotIn("IMPORT_NAME_INJECTED", preprocessed.stdout)
                return combined

            normal = generate_and_compile("normal", hostile)
            self.assertEqual(normal.count(f"import: {encoded}"), 1)

            extra_hostile = tmp / "extra-hostile.prx"
            extra_blob, _ = build_synthetic_import_prx(payload, extra_base)
            extra_hostile.write_bytes(extra_blob)
            extra = generate_and_compile("extra", ordinary, extra_hostile)
            self.assertIn("import: sceDisplay nid 0x12345678", extra)
            self.assertEqual(extra.count(f"import: {encoded}"), 1)


if __name__ == "__main__":
    unittest.main()
