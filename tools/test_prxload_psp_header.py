# SPDX-License-Identifier: GPL-3.0-or-later

from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import prxload


def make_elf(path, filesz=4, memsz=4):
    data = bytearray(0x104)
    data[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into("<III", data, 24, 0, 52, 0)
    struct.pack_into("<HHHHH", data, 42, 32, 1, 0, 0, 0)
    struct.pack_into("<8I", data, 52, 1, 0x100, 0, 0, filesz, memsz, 7, 16)
    data[0x100:0x104] = b"DATA"
    path.write_bytes(data)


def make_psp_header(path, segment_size=16, bss_size=12, segment_count=1,
                    elf_size=0, psp_size=0, segment_sizes=None):
    data = bytearray(0x80)
    data[:4] = b"~PSP"
    data[0x27] = segment_count
    struct.pack_into("<i", data, 0x28, elf_size)
    struct.pack_into("<i", data, 0x2C, psp_size)
    struct.pack_into("<i", data, 0x38, bss_size)
    if segment_sizes is None:
        segment_sizes = [segment_size] + [0] * (4 - segment_count)
    else:
        segment_sizes = list(segment_sizes) + [0] * (4 - len(segment_sizes))
    struct.pack_into("<4i", data, 0x54, *segment_sizes[:4])
    path.write_bytes(data)


class PspHeaderImageExtentTests(unittest.TestCase):
    def test_psp_segment_size_restores_missing_bss(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            elf = root / "module.elf"
            header = root / "EBOOT.BIN"
            make_elf(elf)
            make_psp_header(header)

            module = prxload.Prx(elf, 0, psp_header=header)

            self.assertEqual(len(module.mem), 16)
            self.assertEqual(module.mem[:4], b"DATA")
            self.assertEqual(module.mem[4:], bytes(12))
            self.assertEqual(module.psp_bss_size, 12)

    def test_invalid_psp_header_is_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            elf = root / "module.elf"
            header = root / "EBOOT.BIN"
            make_elf(elf)
            header.write_bytes(bytes(0x80))

            with self.assertRaisesRegex(ValueError, "not a valid ~PSP"):
                prxload.Prx(elf, 0, psp_header=header)

    def test_header_cannot_declare_less_memory_than_file_bytes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            elf = root / "module.elf"
            header = root / "EBOOT.BIN"
            make_elf(elf)
            make_psp_header(header, segment_size=3, bss_size=0)

            with self.assertRaisesRegex(ValueError, "smaller than ELF file size"):
                prxload.Prx(elf, 0, psp_header=header)

    def test_psp_header_rejects_negative_bss_size(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            elf = root / "module.elf"
            header = root / "EBOOT.BIN"
            make_elf(elf)
            make_psp_header(header, bss_size=-1)

            with self.assertRaisesRegex(ValueError, "invalid BSS size"):
                prxload.Prx(elf, 0, psp_header=header)

    def test_psp_header_rejects_negative_segment_size(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            elf = root / "module.elf"
            header = root / "EBOOT.BIN"
            make_elf(elf)
            make_psp_header(header, segment_sizes=[-1])

            with self.assertRaisesRegex(ValueError, "invalid memory size"):
                prxload.Prx(elf, 0, psp_header=header)

    def test_psp_header_rejects_elf_size_exceeding_psp_size(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            elf = root / "module.elf"
            header = root / "EBOOT.BIN"
            make_elf(elf)
            make_psp_header(header, elf_size=0x2000, psp_size=0x1000)

            with self.assertRaisesRegex(ValueError, "exceeds container size"):
                prxload.Prx(elf, 0, psp_header=header)

    def test_psp_header_rejects_psp_size_smaller_than_header(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            elf = root / "module.elf"
            header = root / "EBOOT.BIN"
            make_elf(elf)
            make_psp_header(header, psp_size=0x10)

            with self.assertRaisesRegex(ValueError, "smaller than the minimum ~PSP header size"):
                prxload.Prx(elf, 0, psp_header=header)

    def test_psp_header_rejects_segment_sizes_less_than_bss(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            elf = root / "module.elf"
            header = root / "EBOOT.BIN"
            make_elf(elf)
            make_psp_header(header, segment_sizes=[10], bss_size=100)

            with self.assertRaisesRegex(ValueError, "do not cover declared BSS size"):
                prxload.Prx(elf, 0, psp_header=header)


if __name__ == "__main__":
    unittest.main()
