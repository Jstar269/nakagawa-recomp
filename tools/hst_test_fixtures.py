# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Small source-owned fixtures shared by HST doctor tests.

The builders emit deliberately minimal ELF, PSP-header, and ISO envelopes;
they contain no retail bytes and are kept independent of the doctor under test.
"""

from __future__ import annotations

from pathlib import Path
import struct


def write_elf(path: Path, *, machine: int = 8, load_segments: int = 1) -> None:
    """Write a minimal ELF32 image with ``load_segments`` executable loads."""
    if load_segments < 1:
        raise ValueError("load_segments must be positive")
    phoff = 52
    phentsize = 32
    data_offset = phoff + phentsize * load_segments
    payload = b"\0" * 16
    header = bytearray(52)
    header[:4] = b"\x7fELF"
    header[4] = 1  # ELF32
    header[5] = 1  # little-endian
    header[6] = 1
    struct.pack_into("<HHI", header, 16, 2, machine, 1)
    struct.pack_into("<III", header, 24, 0, phoff, 0)
    struct.pack_into("<I", header, 36, 0)
    struct.pack_into("<HHHHHH", header, 40, 52, phentsize, load_segments, 40, 0, 0)
    phdrs = bytearray()
    for index in range(load_segments):
        offset = data_offset + index * len(payload)
        phdrs.extend(
            struct.pack(
                "<8I",
                1,
                offset,
                index * 0x1000,
                index * 0x1000,
                len(payload),
                len(payload),
                5,
                0x10,
            )
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(header) + bytes(phdrs) + payload * load_segments)


def write_psp_header(path: Path, *, segments: int = 1) -> None:
    """Write a minimal ``~PSP`` header with bounded segment-size fields."""
    if not 1 <= segments <= 4:
        raise ValueError("segments must be between 1 and 4")
    data = bytearray(0x80)
    data[:4] = b"~PSP"
    data[0x27] = segments
    struct.pack_into("<I", data, 0x38, 0x100)
    sizes = [0x1000] * segments + [0] * (4 - segments)
    struct.pack_into("<4I", data, 0x54, *sizes)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def write_iso(
    path: Path,
    *,
    descriptor_type: int = 1,
    disc_id: bytes | None = b"UCUS98701",
) -> None:
    """Write a minimal ISO-like PVD fixture used by doctor validation tests."""
    data = bytearray(20 * 2048)
    pvd = 16 * 2048
    data[pvd] = descriptor_type
    data[pvd + 1 : pvd + 6] = b"CD001"
    data[pvd + 6] = 1
    if disc_id is not None:
        if len(disc_id) > 32:
            raise ValueError("disc_id is too long for the synthetic PVD field")
        data[pvd + 200 : pvd + 200 + len(disc_id)] = disc_id
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
