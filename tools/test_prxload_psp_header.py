# SPDX-License-Identifier: GPL-2.0-or-later

import struct
import tempfile
import unittest
from pathlib import Path

import prxload


def make_elf(path, filesz=4, memsz=4):
    data = bytearray(0x104)
    data[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into("<III", data, 24, 0, 52, 0)
    struct.pack_into("<HHHHH", data, 42, 32, 1, 0, 0, 0)
    struct.pack_into("<8I", data, 52, 1, 0x100, 0, 0, filesz, memsz, 7, 16)
    data[0x100:0x104] = b"DATA"
    path.write_bytes(data)


def make_psp_header(path, segment_size=16, bss_size=12, segment_count=1):
    data = bytearray(0x80)
    data[:4] = b"~PSP"
    data[0x27] = segment_count
    struct.pack_into("<I", data, 0x38, bss_size)
    struct.pack_into("<I", data, 0x54, segment_size)
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


if __name__ == "__main__":
    unittest.main()
