# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Synthetic PSP ELF fixtures for the import-coverage audit gate.

Everything here is generated from scratch: library names, NIDs, and layout are
invented public values, never extracted from a game binary. Fixtures are built
in memory as bytes, so no ELF binary is ever committed (the repository ignores
and publication-audits *.elf). Layouts are deterministic.

The well-formed builder produces a minimal ELF32 MIPS executable with one
PT_LOAD segment, a .rodata.sceModuleInfo section, and a PspLibStubEntry table,
which is exactly the surface tools/psp_import_table.py consumes. Malformed
variants each corrupt one property the parser must reject cleanly.
"""

from __future__ import annotations

import struct

BASE_VADDR = 0x08804000
DATA_FILE_OFF = 0x1000


def _elf(
    segment: bytes,
    modinfo_vaddr: int,
    *,
    truncate_to: int | None = None,
    sectionless: bool = False,
    paddr_override: int | None = None,
    extra_sections: list[tuple[bytes, int, int]] | None = None,
) -> bytes:
    """Wrap a guest segment into a minimal ELF32 MIPS file.

    sectionless=True emits no section headers at all (a stripped PRX-style
    input); SceModuleInfo is then located via the PRX convention, so
    phdr[0].p_paddr carries the record's file offset (or paddr_override).

    extra_sections appends named SHT_PROGBITS/SHF_ALLOC section headers in
    (name, vaddr, size) order, so fixtures can carry the real
    .sceStub.text/.rodata.sceNid pairing sections.
    """
    e_phoff = 52
    e_shoff_placeholder = 0
    modinfo_file_off = DATA_FILE_OFF + (modinfo_vaddr - BASE_VADDR)
    extra_sections = extra_sections or []
    if sectionless:
        p_paddr = modinfo_file_off if paddr_override is None else paddr_override
        e_shnum, e_shstrndx = 0, 0
    else:
        p_paddr = BASE_VADDR
        e_shnum, e_shstrndx = 3 + len(extra_sections), 2 + len(extra_sections)
    ehdr = struct.pack(
        "<4s5B7x2H5I6H",
        b"\x7fELF", 1, 1, 1, 0, 0,      # ELFCLASS32, ELFDATA2LSB, EV_CURRENT
        2, 8,                            # ET_EXEC, EM_MIPS
        1,                               # e_version
        BASE_VADDR,                      # e_entry
        e_phoff, e_shoff_placeholder, 0,  # e_phoff, e_shoff (patched), e_flags
        52, 32, 1,                       # e_ehsize, e_phentsize, e_phnum
        40, e_shnum, e_shstrndx,         # e_shentsize, e_shnum, e_shstrndx
    )
    phdr = struct.pack(
        "<8I",
        1,                               # PT_LOAD
        DATA_FILE_OFF, BASE_VADDR, p_paddr,
        len(segment), len(segment),      # p_filesz, p_memsz
        7, 0x1000,                       # rwx, align
    )
    pad = b"\0" * (DATA_FILE_OFF - len(ehdr) - len(phdr))

    if sectionless:
        out = bytes(ehdr + phdr + pad + segment)
        if truncate_to is not None:
            out = out[:truncate_to]
        return out

    shstrtab = b"\0.rodata.sceModuleInfo\0.shstrtab\0"
    extra_name_offs = {}
    for nm, _vaddr, _size in extra_sections:
        extra_name_offs[nm] = len(shstrtab)
        shstrtab += nm + b"\0"
    shstrtab_off = DATA_FILE_OFF + len(segment)
    e_shoff = shstrtab_off + len(shstrtab)
    sh_null = struct.pack("<10I", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    sh_modinfo = struct.pack(
        "<10I", 1, 1, 2,                 # name, SHT_PROGBITS, SHF_ALLOC
        modinfo_vaddr, DATA_FILE_OFF + (modinfo_vaddr - BASE_VADDR), 52,
        0, 0, 4, 0,
    )
    sh_extras = []
    for nm, vaddr, size in extra_sections:
        sh_extras.append(
            struct.pack(
                "<10I", extra_name_offs[nm], 1, 2,
                vaddr, DATA_FILE_OFF + (vaddr - BASE_VADDR), size,
                0, 0, 4, 0,
            )
        )
    sh_shstr = struct.pack("<10I", 23, 3, 0, 0, shstrtab_off, len(shstrtab), 0, 0, 1, 0)

    blob = bytearray(
        ehdr + phdr + pad + segment + shstrtab + sh_null + sh_modinfo
        + b"".join(sh_extras) + sh_shstr
    )
    struct.pack_into("<I", blob, 32, e_shoff)
    out = bytes(blob)
    if truncate_to is not None:
        out = out[:truncate_to]
    return out


def build_import_elf(
    libs: list[tuple[str, list[int]]],
    *,
    entry_size_words: int = 5,
    corrupt: str | None = None,
    sectionless: bool = False,
) -> bytes:
    """Build a synthetic ELF whose import table lists `libs` as (name, [NIDs]).

    sectionless=True omits every section header; the parser must then locate
    SceModuleInfo via phdr[0].p_paddr (the stripped-PRX convention).

    corrupt values (each produces exactly one malformed property; the ELF
    envelope and module-info location stay valid unless stated):
      "truncated_file"      -- file cut mid stub table (envelope truncated too)
      "zero_entry_size"     -- first PspLibStubEntry.size == 0
      "entry_overrun"       -- entry size runs past libstubend
      "entry_header_truncated" -- libstubend cuts the first entry header short
      "wrapped_nid_table"   -- nidData near 0xffffffff so the array wraps
      "wrapped_stub_area"   -- firstSym near 0xffffffff so stubs wrap
      "bad_name_ptr"        -- library name pointer outside every load range
      "unterminated_name"   -- name never NUL-terminates within the cap
      "null_nid_table"      -- numFuncs > 0 with nidData == 0
      "nid_table_partially_backed"  -- NID array crosses the end of the load range
      "stub_table_partially_backed" -- function stub span crosses the end of the load range
      "stub_area_unmapped"  -- firstSym points outside every load range
      "stub_area_misaligned"-- firstSym not 4-byte aligned
      "stub_range_reversed" -- libstub above libstubend
      "stubend_past_segment"-- libstubend beyond the loaded segment
      "sectionless_bad_paddr" -- sectionless input whose phdr[0].p_paddr file
                                 offset is outside every loaded file range
    """
    if corrupt == "sectionless_bad_paddr":
        sectionless = True
    seg = bytearray()

    def alloc(b: bytes, align: int = 4) -> int:
        while len(seg) % align:
            seg.append(0)
        off = len(seg)
        seg.extend(b)
        return BASE_VADDR + off

    modinfo_vaddr = alloc(b"\0" * 52)

    name_vaddrs = []
    for name, _nids in libs:
        raw = name.encode("ascii") + b"\0"
        if corrupt == "unterminated_name":
            raw = b"A" * 512  # no NUL; runs into subsequent data
        name_vaddrs.append(alloc(raw, 1))

    nid_vaddrs = []
    for _name, nids in libs:
        nid_vaddrs.append(alloc(b"".join(struct.pack("<I", n) for n in nids)))

    stub_vaddrs = []
    for _name, nids in libs:
        stub_vaddrs.append(alloc(b"\0" * (8 * len(nids))))

    entries = bytearray()
    for i, (name, nids) in enumerate(libs):
        name_ptr = name_vaddrs[i]
        nid_data = nid_vaddrs[i]
        first_sym = stub_vaddrs[i]
        size_words = entry_size_words
        if i == 0:
            if corrupt == "zero_entry_size":
                size_words = 0
            elif corrupt == "entry_overrun":
                size_words = 31
            elif corrupt == "wrapped_nid_table":
                nid_data = 0xFFFFFFFC
            elif corrupt == "wrapped_stub_area":
                first_sym = 0xFFFFFFF8
            elif corrupt == "bad_name_ptr":
                name_ptr = 0x00100000
            elif corrupt == "null_nid_table":
                nid_data = 0
        entries += struct.pack(
            "<IHHBBHII", name_ptr, 0x0101, 0x0009, size_words, 0, len(nids), nid_data, first_sym
        )
        # entry_overrun claims a large size but emits only the 20-byte header,
        # so the claimed extent genuinely runs past libstubend.
        if corrupt != "entry_overrun":
            entries += b"\0" * (size_words * 4 - 20 if size_words * 4 > 20 else 0)

    libstub = alloc(bytes(entries))
    libstubend = libstub + len(entries)
    if corrupt == "stub_range_reversed":
        libstub, libstubend = libstubend, libstub
    elif corrupt == "stubend_past_segment":
        libstubend = libstub + len(entries) + 0x100
    elif corrupt == "entry_header_truncated":
        libstubend = libstub + 10

    struct.pack_into("<II", seg, (modinfo_vaddr - BASE_VADDR) + 44, libstub, libstubend)

    # Post-layout patches to the first entry's pointer fields: these need the
    # final segment length, so the envelope and every other field stay valid.
    entry0_off = libstub - BASE_VADDR
    seg_end_vaddr = BASE_VADDR + len(seg)
    if corrupt == "nid_table_partially_backed":
        struct.pack_into("<I", seg, entry0_off + 12, seg_end_vaddr - 4)
    elif corrupt == "stub_table_partially_backed":
        struct.pack_into("<I", seg, entry0_off + 16, seg_end_vaddr - 8)
    elif corrupt == "stub_area_unmapped":
        struct.pack_into("<I", seg, entry0_off + 16, 0x00100000)
    elif corrupt == "stub_area_misaligned":
        struct.pack_into("<I", seg, entry0_off + 16, stub_vaddrs[0] + 2)

    truncate_to = None
    if corrupt == "truncated_file":
        truncate_to = DATA_FILE_OFF + (libstub - BASE_VADDR) + 10
    return _elf(
        bytes(seg),
        modinfo_vaddr,
        truncate_to=truncate_to,
        sectionless=sectionless,
        paddr_override=4 if corrupt == "sectionless_bad_paddr" else None,
    )


# The real-world interleaved stub-table shape that the per-window span walk
# misparsed (it recovered only the 35 slots inside window runs instead of all
# 51 global slots). One PspLibStubEntry per library names a *run* of global
# positions (first slot, numFuncs); because archive interleaving scatters one
# library's slots across the table while the entry's numFuncs counts the
# library's whole total, runs overlap and positions outside every run exist.
# Library names are public PSPSDK names; NIDs stay synthetic in fixtures.
INTERLEAVED_SHAPE = [
    ("sceDisplay", 0, 2),
    ("sceGe_user", 2, 1),
    ("IoFileMgrForUser", 3, 11),
    ("ModuleMgrForUser", 7, 1),
    ("ThreadManForUser", 8, 18),
    ("LoadExecForUser", 11, 1),
    ("StdioForKernel", 12, 1),
    ("SysclibForKernel", 13, 2),
    ("sceUtility", 15, 1),
    ("sceNetInet", 16, 4),
    ("Kernel_Library", 27, 2),
    ("StdioForUser", 29, 3),
    ("SysMemUserForUser", 32, 4),
]

# One synthetic NID per global slot (51 slots; 0x0F000000..0x0F000032).
INTERLEAVED_NIDS = [0x0F000000 + i for i in range(51)]


def build_interleaved_import_elf(
    windows: list[tuple[str, int, int]],
    nids: list[int],
    *,
    sectionless: bool = False,
    corrupt: str | None = None,
) -> bytes:
    """Build a synthetic ELF with an interleaved (psp-fixup-imports style)
    import table.

    Unlike build_import_elf, there is exactly one global 8-byte stub slot and
    one 4-byte NID per position: slot k pairs with NID k, and each
    PspLibStubEntry names a run of consecutive positions (first slot, count).
    Runs from different libraries may overlap and positions outside every run
    are never patched by the loader. When section headers are emitted, the
    real .sceStub.text / .rodata.sceNid sections bound the pairing regions.

    corrupt values:
      "nid_region_mismatch" -- the NID region outlives the paired stub region
          by one word (a stray trailing NID with sections; a variable-only
          stub slot past every function run without sections). The 1:1
          stub-slot/NID pairing check must fail closed.
    """
    total = len(nids)
    if total <= 0:
        raise ValueError("interleaved fixture needs at least one NID")
    seg = bytearray()

    def alloc(b: bytes, align: int = 4) -> int:
        while len(seg) % align:
            seg.append(0)
        off = len(seg)
        seg.extend(b)
        return BASE_VADDR + off

    modinfo_vaddr = alloc(b"\0" * 52)
    name_vaddrs = [alloc(name.encode("ascii") + b"\0", 1) for name, _first, _count in windows]
    nid_array = alloc(b"".join(struct.pack("<I", n) for n in nids))
    nid_sec_size = 4 * total
    if corrupt == "nid_region_mismatch":
        if sectionless:
            # A variable-only entry (numFuncs=0, numVars=1) claims a stub slot
            # one position past every function run: the stub region grows by
            # one slot while the NID region stays put.
            windows = windows + [("(variable)", total, 0)]
            name_vaddrs.append(alloc(b"(variable)\0", 1))
        else:
            # A stray trailing NID word: the .rodata.sceNid section extends
            # past the stub region's paired extent.
            nid_sec_size = 4 * (total + 1)
            alloc(struct.pack("<I", 0xDEADBEEF))
    first_sym = alloc(b"\0" * (8 * total))

    entries = bytearray()
    for (name, first, count), name_ptr in zip(windows, name_vaddrs):
        if count == 0:
            # Variable-only entry: 6 words (20-byte header plus one word for
            # the variable list), no NID pointer (numFuncs == 0).
            entries += (
                struct.pack(
                    "<IHHBBHII", name_ptr, 0x0101, 0x0009, 6, 1, 0, 0,
                    first_sym + first * 8,
                )
                + b"\0" * 4
            )
            continue
        entries += struct.pack(
            "<IHHBBHII", name_ptr, 0x0101, 0x0009, 5, 0, count,
            nid_array + first * 4, first_sym + first * 8,
        )
    libstub = alloc(bytes(entries))
    libstubend = libstub + len(entries)
    struct.pack_into("<II", seg, (modinfo_vaddr - BASE_VADDR) + 44, libstub, libstubend)

    extra_sections = None
    if not sectionless:
        extra_sections = [
            (b".sceStub.text", first_sym, 8 * total),
            (b".rodata.sceNid", nid_array, nid_sec_size),
        ]
    return _elf(
        bytes(seg),
        modinfo_vaddr,
        sectionless=sectionless,
        extra_sections=extra_sections,
    )


# The mixed-classification fixture library set used by tests, the CI gate, and
# docs examples. NIDs are synthetic except where a real public NID is needed
# to exercise a manifest classification (those NIDs and API names are public
# PSPSDK/PPSSPP knowledge and already appear in src/rt/hle.c).
MIXED_FIXTURE_LIBS = [
    # dedicated (real handler), fake_success (h_ok), controlled_unsupported
    ("ThreadManForUser", [0x446D8DE6, 0x349D6D6C]),
    ("scePsmfPlayer", [0x46F61F8B]),
    # missing: nobody registers these synthetic NIDs
    ("SynthLibA", [0x00C0FFEE, 0x0BADF00D]),
    # duplicate NID imported by two different libraries
    ("SynthLibB", [0x0BADF00D]),
]
