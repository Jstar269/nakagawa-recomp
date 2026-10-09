# SPDX-License-Identifier: GPL-3.0-or-later
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
    e_type: int = 2,
    base_vaddr: int = BASE_VADDR,
) -> bytes:
    """Wrap a guest segment into a minimal ELF32 MIPS file.

    sectionless=True emits no section headers at all (a stripped PRX-style
    input); SceModuleInfo is then located via the PRX convention, so
    phdr[0].p_paddr carries the record's file offset (or paddr_override).

    extra_sections appends named SHT_PROGBITS/SHF_ALLOC section headers in
    (name, vaddr, size) order, so fixtures can carry the real
    .sceStub.text/.rodata.sceNid pairing sections.

    e_type and base_vaddr default to an ET_EXEC loaded at BASE_VADDR; a
    relocatable PRX fixture passes e_type=0xFFA0 and base_vaddr=0.
    """
    e_phoff = 52
    e_shoff_placeholder = 0
    modinfo_file_off = DATA_FILE_OFF + (modinfo_vaddr - base_vaddr)
    extra_sections = extra_sections or []
    if sectionless:
        p_paddr = modinfo_file_off if paddr_override is None else paddr_override
        e_shnum, e_shstrndx = 0, 0
    else:
        p_paddr = base_vaddr
        e_shnum, e_shstrndx = 3 + len(extra_sections), 2 + len(extra_sections)
    ehdr = struct.pack(
        "<4s5B7x2H5I6H",
        b"\x7fELF", 1, 1, 1, 0, 0,      # ELFCLASS32, ELFDATA2LSB, EV_CURRENT
        e_type, 8,                       # e_type, EM_MIPS
        1,                               # e_version
        base_vaddr,                      # e_entry
        e_phoff, e_shoff_placeholder, 0,  # e_phoff, e_shoff (patched), e_flags
        52, 32, 1,                       # e_ehsize, e_phentsize, e_phnum
        40, e_shnum, e_shstrndx,         # e_shentsize, e_shnum, e_shstrndx
    )
    phdr = struct.pack(
        "<8I",
        1,                               # PT_LOAD
        DATA_FILE_OFF, base_vaddr, p_paddr,
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
        modinfo_vaddr, DATA_FILE_OFF + (modinfo_vaddr - base_vaddr), 52,
        0, 0, 4, 0,
    )
    sh_extras = []
    for nm, vaddr, size in extra_sections:
        sh_extras.append(
            struct.pack(
                "<10I", extra_name_offs[nm], 1, 2,
                vaddr, DATA_FILE_OFF + (vaddr - base_vaddr), size,
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
    module_attributes: int = 0,
) -> bytes:
    """Build a synthetic ELF whose import table lists `libs` as (name, [NIDs]).

    sectionless=True omits every section header; the parser must then locate
    SceModuleInfo via phdr[0].p_paddr (the stripped-PRX convention).

    module_attributes is written to SceModuleInfo.attribute (0x1000 marks a
    kernel-mode module).

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

    modinfo_vaddr = alloc(struct.pack("<H", module_attributes) + b"\0" * 50)

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
    for i, (_name, nids) in enumerate(libs):
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
    for (_name, first, count), name_ptr in zip(windows, name_vaddrs, strict=True):
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


# The nameless syslib entry every PSP module exports: module_start and the
# module_info variable, under their public PSPSDK lifecycle NIDs.
SYSLIB_MODULE_START_NID = 0xD632ACDB
SYSLIB_MODULE_INFO_NID = 0xF01D73A7
SYSLIB_EXPORT = (None, 0x8000, [SYSLIB_MODULE_START_NID], [SYSLIB_MODULE_INFO_NID])


def build_module_elf(
    exports: list[tuple[str | None, int, list[int], list[int]]],
    *,
    imports: list[tuple[str, list[int]]] | None = None,
    module_attributes: int = 0,
    e_type: int = 0xFFA0,
    base_vaddr: int = 0,
    sectionless: bool = False,
    corrupt: str | None = None,
) -> bytes:
    """Build a synthetic PSP module with an export table and optional imports.

    exports lists SceLibraryEntryTable entries in order as (library name, or
    None for the nameless syslib entry; attribute; function NIDs; variable
    NIDs). Each entry is the 16-byte header the loader reads; its entry table
    holds the function NIDs, then the variable NIDs, then one guest address
    per export. imports, when given, adds a psp-fixup-imports style stub
    table (one 8-byte slot per NID) with .sceStub.text/.rodata.sceNid
    sections. The defaults model a relocatable PRX at base 0; a fixed-address
    module or main executable passes e_type=2 and its load address.

    corrupt values (each breaks exactly one export-table property):
      "entry_too_short"         -- first entry declares 3 words, below the header
      "entry_overrun"           -- first entry's length runs past ent_end
      "entry_table_unmapped"    -- first entry's entry table is outside the segment
      "address_table_truncated" -- last entry's addresses run past the segment end
      "ent_range_reversed"      -- ent_top above ent_end
      "bad_name_ptr"            -- first named entry's name pointer is unmapped
    """
    seg = bytearray()

    def alloc(b: bytes, align: int = 4) -> int:
        while len(seg) % align:
            seg.append(0)
        off = len(seg)
        seg.extend(b)
        return base_vaddr + off

    modinfo_vaddr = alloc(b"\0" * 52)
    code_vaddr = alloc(struct.pack("<2I", 0x03E00008, 0))  # jr $ra; nop

    name_vaddrs = [
        alloc(name.encode("ascii") + b"\0", 1) if name is not None else 0
        for name, _attr, _funcs, _vars in exports
    ]
    table_vaddrs = []
    for _name, _attr, funcs, variables in exports:
        nids = list(funcs) + list(variables)
        addresses = [code_vaddr] * len(nids)
        table_vaddrs.append(
            alloc(b"".join(struct.pack("<I", value) for value in nids + addresses)) if nids else 0
        )
    entries = bytearray()
    for (_name, attr, funcs, variables), name_ptr, table in zip(
        exports, name_vaddrs, table_vaddrs, strict=True
    ):
        entries += struct.pack(
            "<IHHBBHI", name_ptr, 0x0101, attr, 4, len(variables), len(funcs), table
        )
    ent_top = alloc(bytes(entries))
    ent_end = ent_top + len(entries)

    stub_top = stub_end = 0
    extra_sections = []
    if imports:
        import_names = [alloc(name.encode("ascii") + b"\0", 1) for name, _nids in imports]
        all_nids = [nid for _name, nids in imports for nid in nids]
        nid_array = alloc(b"".join(struct.pack("<I", nid) for nid in all_nids))
        first_sym = alloc(b"\0" * (8 * len(all_nids)))
        stub_entries = bytearray()
        position = 0
        for (_name, nids), name_ptr in zip(imports, import_names, strict=True):
            stub_entries += struct.pack(
                "<IHHBBHII", name_ptr, 0x0101, 0x0009, 5, 0, len(nids),
                nid_array + position * 4, first_sym + position * 8,
            )
            position += len(nids)
        stub_top = alloc(bytes(stub_entries))
        stub_end = stub_top + len(stub_entries)
        extra_sections = [
            (b".sceStub.text", first_sym, 8 * len(all_nids)),
            (b".rodata.sceNid", nid_array, 4 * len(all_nids)),
        ]

    entry0 = ent_top - base_vaddr
    if corrupt == "entry_too_short":
        seg[entry0 + 8] = 3
    elif corrupt == "entry_overrun":
        seg[entry0 + 8] = 0x40
    elif corrupt == "entry_table_unmapped":
        struct.pack_into("<I", seg, entry0 + 12, base_vaddr + 0x00100000)
    elif corrupt == "address_table_truncated":
        last = entry0 + 16 * (len(exports) - 1)
        _name, _attr, funcs, variables = exports[-1]
        count = len(funcs) + len(variables)
        struct.pack_into("<I", seg, last + 12, base_vaddr + len(seg) - count * 4)
    elif corrupt == "ent_range_reversed":
        ent_top, ent_end = ent_end, ent_top
    elif corrupt == "bad_name_ptr":
        named = next(index for index, (name, *_rest) in enumerate(exports) if name is not None)
        struct.pack_into("<I", seg, entry0 + 16 * named, base_vaddr + 0x00100000)

    struct.pack_into(
        "<HH28sI4I", seg, modinfo_vaddr - base_vaddr,
        module_attributes, 0x0101, b"SynthModule", base_vaddr + 0x8000,
        ent_top, ent_end, stub_top, stub_end,
    )
    return _elf(
        bytes(seg),
        modinfo_vaddr,
        sectionless=sectionless,
        extra_sections=None if sectionless else extra_sections,
        e_type=e_type,
        base_vaddr=base_vaddr,
    )


# The mixed-classification fixture library set used by tests, the CI gate, and
# docs examples. NIDs are synthetic except where a real public NID is needed
# to exercise a manifest classification (those NIDs and API names are public
# PSPSDK/PPSSPP knowledge and already appear in src/rt/hle.c).
MIXED_FIXTURE_LIBS = [
    # dedicated (real handler), fake_success (h_ok), controlled_unsupported
    ("ThreadManForUser", [0x446D8DE6, 0x349D6D6C]),
    # dedicated: the PSMF video getter drives the media producer and host codecs
    ("scePsmfPlayer", [0x46F61F8B]),
    # controlled unsupported: ATRAC3 voice setter with no source codec
    ("sceSasCore", [0x4AA9EAD6]),
    # missing: nobody registers these synthetic NIDs
    ("SynthLibA", [0x00C0FFEE, 0x0BADF00D]),
    # duplicate NID imported by two different libraries
    ("SynthLibB", [0x0BADF00D]),
]


def build_import_layout_elf(
    primary: list[tuple[str, list[int]]],
    *,
    detached: list[tuple[str, list[int]]] | None = None,
    nid_head_words: int = 0,
    nid_tail_words: int = 0,
    per_library_stub_sections: bool = False,
    reverse_primary_layout: bool = False,
    detached_gap_slots: int | None = None,
    stub_head_slots: int = 0,
    stub_tail_slots: int = 0,
) -> tuple[bytes, dict[int, tuple[str, int]]]:
    """Build a sectioned ELF for the retail import-table layouts.

    primary lists (library, [NIDs]) windows whose stubs live in the stub
    section(s) and whose NIDs live in .rodata.sceNid. Options model the real
    linker layouts the code-generation import map has to accept:

      nid_head_words / nid_tail_words -- zero words before / after the
          window-referenced run inside .rodata.sceNid;
      per_library_stub_sections -- one .sceStub.text.<library> section per
          window instead of a single .sceStub.text;
      reverse_primary_layout -- the linker placed the libraries' stub and NID
          runs in reverse table order;
      detached -- (library, [NIDs]) windows whose stubs sit inside .text and
          whose NIDs follow their library name outside .rodata.sceNid;
      detached_gap_slots -- instead, emit the detached NIDs as one array and
          leave this many unused stub slots (and zero NID words) between
          consecutive detached windows, so they share one pairing offset;
      stub_head_slots / stub_tail_slots -- zero stub slots that no window
          claims before / after the windows inside a single .sceStub.text.

    Returns (ELF bytes, expected {stub address: (library, NID)}).
    """
    detached = detached or []
    seg = bytearray()

    def alloc(b: bytes, align: int = 4) -> int:
        while len(seg) % align:
            seg.append(0)
        off = len(seg)
        seg.extend(b)
        return BASE_VADDR + off

    gap = detached_gap_slots or 0
    # .text is filled with 8-byte (two-word) `jr $ra; nop` pairs: one pair of
    # ordinary code at its start, then two words per detached stub slot
    # (including the unused gap slots after each run; the runs start at
    # text + 8), then one trailing pair of ordinary code after the last run.
    text_words = 4 + 2 * sum(len(nids) + gap for _name, nids in detached)
    text = alloc(struct.pack("<2I", 0x03E00008, 0) * (text_words // 2))
    expected: dict[int, tuple[str, int]] = {}
    detached_runs = []
    cursor = text + 8
    for name, nids in detached:
        detached_runs.append((name, nids, cursor))
        cursor += 8 * (len(nids) + gap)

    layout_order = list(range(len(primary)))
    if reverse_primary_layout:
        layout_order.reverse()
    stub_section = alloc(b"\0" * (8 * stub_head_slots))
    stub_addrs = {}
    stub_sections = []
    for i in layout_order:
        name, nids = primary[i]
        stub_addrs[i] = alloc(b"\0" * (8 * len(nids)))
        stub_sections.append((f".sceStub.text.{name}".encode("ascii"), stub_addrs[i], 8 * len(nids)))
    alloc(b"\0" * (8 * stub_tail_slots))
    stub_section_end = BASE_VADDR + len(seg)

    modinfo = alloc(b"\0" * 52)
    name_addrs = [alloc(name.encode("ascii") + b"\0") for name, _nids in primary]
    if detached_gap_slots is None:
        detached_name_addrs = []
        detached_nid_addrs = []
        for name, nids, _stub in detached_runs:
            detached_name_addrs.append(alloc(name.encode("ascii") + b"\0"))
            detached_nid_addrs.append(alloc(b"".join(struct.pack("<I", n) for n in nids)))
    else:
        detached_name_addrs = [alloc(name.encode("ascii") + b"\0") for name, _n, _s in detached_runs]
        detached_nid_addrs = [
            alloc(b"".join(struct.pack("<I", n) for n in nids) + b"\0" * (4 * gap))
            for _name, nids, _stub in detached_runs
        ]
    detached_meta = []
    for (name, nids, stub), name_addr, nid_addr in zip(
            detached_runs, detached_name_addrs, detached_nid_addrs, strict=True):
        detached_meta.append((name_addr, nids, nid_addr, stub))
        for k, nid in enumerate(nids):
            expected[stub + 8 * k] = (name, nid)

    nid_section = alloc(b"\0" * (4 * nid_head_words))
    nid_addrs = {}
    for i in layout_order:
        _name, nids = primary[i]
        nid_addrs[i] = alloc(b"".join(struct.pack("<I", n) for n in nids))
    alloc(b"\0" * (4 * nid_tail_words))
    nid_section_end = BASE_VADDR + len(seg)
    for i, (name, nids) in enumerate(primary):
        for k, nid in enumerate(nids):
            expected[stub_addrs[i] + 8 * k] = (name, nid)

    entries = bytearray()
    for i, (_name, nids) in enumerate(primary):
        entries += struct.pack(
            "<IHHBBHII", name_addrs[i], 0x0011, 0x4001, 5, 0, len(nids),
            nid_addrs[i], stub_addrs[i],
        )
    for name_addr, nids, nid_addr, stub in detached_meta:
        entries += struct.pack(
            "<IHHBBHII", name_addr, 0x0000, 0x0009, 5, 0, len(nids), nid_addr, stub,
        )
    libstub = alloc(bytes(entries))
    struct.pack_into("<II", seg, (modinfo - BASE_VADDR) + 44, libstub, libstub + len(entries))

    extra_sections = [(b".text", text, text_words * 4)]
    if per_library_stub_sections:
        extra_sections += stub_sections
    else:
        extra_sections.append((b".sceStub.text", stub_section, stub_section_end - stub_section))
    extra_sections.append((b".rodata.sceNid", nid_section, nid_section_end - nid_section))
    return _elf(bytes(seg), modinfo, extra_sections=extra_sections), expected


def build_stripped_module_elf(
    libs: list[tuple[str, list[int]]],
    *,
    gp: int = 0,
    name: bytes = b"SynthMain",
    decoy: bool = False,
    early_decoy: bool = False,
    kernel_bit: bool = False,
    paddr: int | None = None,
) -> tuple[bytes, dict[int, tuple[str, int]], int]:
    """Build a section-less ET_EXEC whose SceModuleInfo the p_paddr names.

    The segment follows the retail order: code, import stubs, the
    PspLibStubEntry table, SceModuleInfo, library names, NIDs. The record
    carries gp (zero for a module built without $gp-relative data) and name.
    decoy=True places a later plausible but false SceModuleInfo (non-zero gp,
    printable name, file-backed spans) whose import table has a function window
    with a null NID pointer. early_decoy=True places, before the real record, a
    false one with a non-zero gp and a long name whose import span does not
    split into whole SceLibraryStubTable records. kernel_bit sets the
    kernel-mode bit of p_paddr; paddr overrides p_paddr outright.

    Returns (ELF bytes, expected {stub address: (library, NID)}, module-info
    address).
    """
    seg = bytearray()

    def alloc(b: bytes, align: int = 4) -> int:
        while len(seg) % align:
            seg.append(0)
        off = len(seg)
        seg.extend(b)
        return BASE_VADDR + off

    alloc(struct.pack("<2I", 0x03E00008, 0) * 2)
    stub_addrs = [alloc(struct.pack("<2I", 0x03E00008, 0) * len(nids)) for _name, nids in libs]
    libstub = alloc(b"\0" * (20 * len(libs)))
    if early_decoy:
        early = alloc(b"\0" * 52)
        seg[early - BASE_VADDR + 4:early - BASE_VADDR + 15] = b"SynthEarly\0"
        # A 12-byte import span: the first record's length (5 words) overruns it.
        struct.pack_into("<5I", seg, early - BASE_VADDR + 32,
                         0x00004000, 0, 0, libstub, libstub + 12)
    modinfo = alloc(b"\0" * 52)
    if not 0 < len(name) < 28:
        raise ValueError("module name must fit the 28-byte field with its terminator")
    seg[modinfo - BASE_VADDR + 4:modinfo - BASE_VADDR + 5 + len(name)] = name + b"\0"
    name_addrs = [alloc(name.encode("ascii") + b"\0") for name, _nids in libs]
    nid_addrs = [alloc(b"".join(struct.pack("<I", n) for n in nids)) for _name, nids in libs]
    expected: dict[int, tuple[str, int]] = {}
    for i, ((name, nids), name_addr, nid_addr, stub_addr) in enumerate(zip(
            libs, name_addrs, nid_addrs, stub_addrs, strict=True)):
        struct.pack_into(
            "<IHHBBHII", seg, libstub - BASE_VADDR + 20 * i,
            name_addr, 0x0011, 0x4001, 5, 0, len(nids), nid_addr, stub_addr)
        for k, nid in enumerate(nids):
            expected[stub_addr + 8 * k] = (name, nid)
    struct.pack_into("<5I", seg, modinfo - BASE_VADDR + 32, gp, 0, 0, libstub, libstub + 20 * len(libs))
    if decoy:
        decoy_info = alloc(b"\0" * 52)
        decoy_entry = alloc(b"\0" * 20)
        decoy_name = alloc(b"SynthDecoyLibrary\0")
        seg[decoy_info - BASE_VADDR + 4:decoy_info - BASE_VADDR + 16] = b"SynthDecoy\0\0"
        struct.pack_into("<5I", seg, decoy_info - BASE_VADDR + 32,
                         0x00001234, 0, 0, decoy_entry, decoy_entry + 20)
        struct.pack_into("<IHHBBHII", seg, decoy_entry - BASE_VADDR,
                         decoy_name, 0x0011, 0x4001, 5, 0, 4, 0, stub_addrs[0])
    p_paddr = DATA_FILE_OFF + (modinfo - BASE_VADDR)
    if kernel_bit:
        p_paddr |= 0x80000000
    if paddr is not None:
        p_paddr = paddr
    return _elf(bytes(seg), modinfo, sectionless=True, paddr_override=p_paddr), expected, modinfo
