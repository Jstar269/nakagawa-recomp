# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Shared, fail-closed ELF32 envelope checks for the offline PSP tools.

The PRX loader and analyzer both consume user-supplied ELF/PRX files.  Keeping
the fixed-table checks here prevents one parser from accepting a truncated or
forged extent that another parser rejects, and turns incidental ``struct``
exceptions into deterministic ``ValueError`` diagnostics.
"""

from __future__ import annotations

import struct


ELF32_HEADER_SIZE = 52
ELF32_PHDR_SIZE = 32
ELF32_SHDR_SIZE = 40
MAX_ELF_FILE_BYTES = 256 * 1024 * 1024
MAX_ELF_IMAGE_BYTES = 256 * 1024 * 1024
SHF_ALLOC = 0x2
#: Section types a PSP loader reads: SHT_RELA, SHT_REL and the two PSP relocation formats.
LOADER_SECTION_TYPES = frozenset({4, 9, 0x700000A0, 0x700000A1})


def checked_span(total: int, offset: int, size: int, label: str) -> None:
    """Require ``[offset, offset + size)`` to be wholly inside ``total``."""

    if offset < 0 or size < 0 or offset > total or size > total - offset:
        raise ValueError(
            f"{label} out of range (offset=0x{offset:x}, size=0x{size:x}, "
            f"file=0x{total:x})"
        )


def checked_mul(a: int, b: int, label: str, limit: int | None = None) -> int:
    if a < 0 or b < 0:
        raise ValueError(f"{label} has a negative extent")
    value = a * b
    if limit is not None and value > limit:
        raise ValueError(f"{label} exceeds the supported bound (0x{value:x})")
    return value


def validate_elf32_envelope(data: bytes, path: str = "<memory>") -> dict:
    """Validate fixed ELF32 tables and return the decoded envelope fields.

    The returned ``phdrs``/``shdrs`` contain the same little-endian fields used
    by the existing tools.  No payload is interpreted here; callers still own
    the PRX relocation-format checks.

    Two section-table defects that a PSP loader never sees are tolerated and
    reported in ``findings`` instead of refusing the file: a section the loader
    does not read (not allocated, not relocations, not the section-string table)
    whose bytes lie past the end of the file keeps size 0, and a section-string
    table that cannot name the sections turns the table into the unnamed form
    (``shstrndx`` 0, every name 0). Everything a loader reads stays fail-closed.
    """

    total = len(data)
    if total > MAX_ELF_FILE_BYTES:
        raise ValueError(f"{path}: ELF file exceeds the 256 MiB input bound")
    if total < ELF32_HEADER_SIZE:
        raise ValueError(f"{path}: truncated ELF32 header")
    if data[:4] != b"\x7fELF":
        raise ValueError(f"{path}: not an ELF")
    if data[4] != 1 or data[5] != 1:
        raise ValueError(f"{path}: only little-endian ELF32 input is supported")

    e_type, machine = struct.unpack_from("<HH", data, 16)
    entry, phoff, shoff = struct.unpack_from("<III", data, 24)
    phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from(
        "<HHHHH", data, 42
    )

    phdrs = []
    if phnum:
        if phentsize < ELF32_PHDR_SIZE:
            raise ValueError(f"{path}: ELF program-header entry is too small")
        ph_table_size = checked_mul(
            phentsize, phnum, "program-header table", MAX_ELF_FILE_BYTES
        )
        checked_span(total, phoff, ph_table_size, "program-header table")
        for i in range(phnum):
            off = phoff + i * phentsize
            fields = struct.unpack_from("<8I", data, off)
            p_type, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = fields
            if p_type == 1 and p_filesz > p_memsz:
                raise ValueError(f"{path}: PT_LOAD filesz exceeds memsz")
            # Validate the complete source span even when it is empty.  An empty
            # segment still carries a file offset; accepting an offset past EOF
            # lets one consumer accept a structurally impossible envelope while
            # another consumer rejects it later.
            checked_span(total, p_off, p_filesz, f"program segment {i}")
            phdrs.append(
                dict(
                    type=p_type,
                    off=p_off,
                    vaddr=p_vaddr,
                    paddr=p_paddr,
                    filesz=p_filesz,
                    memsz=p_memsz,
                    flags=p_flags,
                    align=p_align,
                    idx=i,
                )
            )

    shdrs = []
    findings = []
    if shnum:
        if shentsize < ELF32_SHDR_SIZE:
            raise ValueError(f"{path}: ELF section-header entry is too small")
        sh_table_size = checked_mul(
            shentsize, shnum, "section-header table", MAX_ELF_FILE_BYTES
        )
        checked_span(total, shoff, sh_table_size, "section-header table")
        if shstrndx >= shnum:
            raise ValueError(f"{path}: section-string-table index is out of range")
        for i in range(shnum):
            off = shoff + i * shentsize
            fields = struct.unpack_from("<10I", data, off)
            name, typ, flags, addr, sec_off, size, link, info, align, entsz = fields
            # SHT_NOBITS occupies no bytes in the file.  All other sections
            # must be fully file-backed before a caller slices them, except one
            # the loader never reads: its absent bytes leave it empty.
            unread = not (flags & SHF_ALLOC) and typ not in LOADER_SECTION_TYPES and i != shstrndx
            if typ != 8 and size:
                try:
                    checked_span(total, sec_off, size, f"section {i}")
                except ValueError:
                    if not unread:
                        raise
                    findings.append(
                        f"section {i} (type 0x{typ:x}, not loaded) ends past the end of the "
                        f"file (offset=0x{sec_off:x}, size=0x{size:x}, file=0x{total:x}); "
                        "it is read as empty"
                    )
                    sec_off, size = min(sec_off, total), 0
            elif sec_off > total:
                if not unread:
                    raise ValueError(f"section {i} offset is outside the file")
                findings.append(
                    f"section {i} (type 0x{typ:x}, not loaded) starts past the end of the "
                    "file; it is read as empty"
                )
                sec_off = total
            shdrs.append(
                dict(
                    name=name,
                    typ=typ,
                    flags=flags,
                    addr=addr,
                    off=sec_off,
                    size=size,
                    link=link,
                    info=info,
                    align=align,
                    entsz=entsz,
                    idx=i,
                )
            )
        if shstrndx == 0:
            if any(section["name"] != 0 for section in shdrs):
                raise ValueError(f"{path}: unnamed section table has nonzero name offsets")
        else:
            shstr = shdrs[shstrndx]
            if shstr["typ"] == 8:
                raise ValueError(f"{path}: section-string table cannot be SHT_NOBITS")
            checked_span(total, shstr["off"], shstr["size"], "section-string table")
            shstr_end = shstr["off"] + shstr["size"]
            unusable = next((
                "a section name offset is out of range"
                if section["name"] >= shstr["size"] else "a section name is unterminated"
                for section in shdrs
                if section["name"] >= shstr["size"]
                or data.find(b"\0", shstr["off"] + section["name"], shstr_end) < 0
            ), None)
            if unusable is not None:
                # Names are bookkeeping no PSP loader reads: the table stays usable
                # by type, address and size, as an unnamed one.
                findings.append(
                    f"section-string table (section {shstrndx}, 0x{shstr['size']:x} bytes) "
                    f"cannot name the sections: {unusable}; the section table is read unnamed"
                )
                shstrndx = 0
                for section in shdrs:
                    section["name"] = 0

    return dict(
        e_type=e_type,
        machine=machine,
        entry=entry,
        phoff=phoff,
        shoff=shoff,
        phentsize=phentsize,
        phnum=phnum,
        shentsize=shentsize,
        shnum=shnum,
        shstrndx=shstrndx,
        phdrs=phdrs,
        shdrs=shdrs,
        findings=findings,
    )


def image_extent(base: int, vaddr: int, memsz: int, label: str) -> tuple[int, int]:
    """Return a checked absolute segment range for a 32-bit guest image."""

    if base < 0 or vaddr < 0 or memsz < 0:
        raise ValueError(f"{label} has a negative address/size")
    start = base + vaddr
    end = start + memsz
    if end < start or end > 0x1_0000_0000:
        raise ValueError(f"{label} exceeds the 32-bit address space")
    if end - base > MAX_ELF_IMAGE_BYTES:
        raise ValueError(f"{label} exceeds the 256 MiB flat-image bound")
    return start, end
