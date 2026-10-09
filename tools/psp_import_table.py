# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Defensive PSP ELF import- and export-table parser.

Used by the import-coverage audit gate and by the guest-module planner.
Standalone on purpose: tools/analyze.py's Elf class is a trusting pipeline
loader for a known-good local ELF, while this module is fed arbitrary
developer-supplied byte buffers (and deliberately malformed CI fixtures).
Every read is bounds-checked against the file, every guest address is mapped
through validated PT_LOAD/section ranges, and every failure raises
ImportTableError with a message instead of crashing, wrapping, or allocating
based on unvalidated lengths. Nothing here executes or disassembles guest
code; the output is (library name, function NID) pairs plus stub addresses
for imports, and (library name, attributes, exported NIDs) for exports.

Layout references: psp-fixup-imports (pspsdk/tools) builds the table the PSP
kernel loader consumes: .sceStub.text holds one 8-byte slot per imported
function, .rodata.sceNid holds one 4-byte NID per function, and the two
arrays pair globally by position (psp-fixup-imports aborts when a slot's
embedded NID differs from the section NID). SceModuleInfo.libstub..libstubend
holds one PspLibStubEntry per library naming a run of numFuncs consecutive
positions. When the linker interleaves stub libraries (the "stubs out of
order" case psp-fixup-imports warns about) the runs overlap and trailing
positions can be left unclaimed; the loader patches only covered positions.
This parser therefore pairs slots with NIDs globally, attributes library
names from the window runs (last claimer wins on overlap), marks unclaimed
slots with the UNATTRIBUTED_LIBRARY marker, and reports structural findings;
it fails closed on malformed bounds, truncated records, overflow, impossible
counts, and windows whose NID position disagrees with their stub position.

Exports use the PSP SceLibraryEntryTable layout the module loader consumes
(the same layout src/rt/hle.c register_prx_exports() publishes when it loads
a guest module): SceModuleInfo.ent_top..ent_end holds one entry per exported
library, each starting with a 16-byte header (library-name pointer, version,
attribute, entry length in words, variable count, function count, entry
table pointer). The entry table holds every function NID, then every
variable NID, then the matching guest addresses in the same order. The
nameless entry carrying the module-lifecycle exports (module_start,
module_info, ...) has the syslib attribute and is never importable.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import struct

# Hard resource caps. A malformed header must not be able to request work or
# memory proportional to a forged length field.
MAX_FILE_SIZE = 256 * 1024 * 1024
MAX_SEGMENTS = 512
MAX_SECTIONS = 512
MAX_LIBRARIES = 256
MAX_FUNCS_PER_LIB = 4096
MAX_TOTAL_FUNCS = 65536
MAX_LIBNAME_LEN = 128
# PspLibStubEntry.size is in 32-bit words; 5 covers the fields we decode and
# real tables use 5 or 6. Anything outside a small window is hostile/corrupt.
MIN_STUB_ENTRY_WORDS = 5
MAX_STUB_ENTRY_WORDS = 32
# SceLibraryEntryTable.len is in 32-bit words; the 16-byte header is the
# minimum and real tables use 4 to 6. The upper bound and the export-table span
# cap are the runtime loader's own (src/rt/hle.c register_prx_exports), so the
# planner never refuses a table the runtime would publish.
MIN_EXPORT_ENTRY_WORDS = 4
MAX_EXPORT_ENTRY_WORDS = 0x40
MAX_EXPORT_TABLE_BYTES = 0x10000
EXPORT_ENTRY_HEADER_SIZE = 16

# SceModuleInfo.modattribute bit for a kernel-mode module.
MODULE_ATTR_KERNEL = 0x1000
# SceLibraryEntryTable.attribute bits. A kernel-mode module's library is
# callable from user mode only through the syscall-export bit; the syslib bit
# marks the nameless module-lifecycle entry, which no module can import.
LIB_ATTR_SYSCALL_EXPORT = 0x4000
LIB_ATTR_SYSLIB = 0x8000

MODULE_INFO_SECTION = b".rodata.sceModuleInfo"
MODULE_INFO_SIZE = 52
STUB_ENTRY_HEADER_SIZE = 20
STUB_SECTION = b".sceStub.text"
NID_SECTION = b".rodata.sceNid"
U32_MAX = 0xFFFFFFFF

# Marker for stub slots that no library window claims (interleaved stub
# tables). Library-name reads are restricted to printable ASCII, so this
# literal can never collide with a real guest name.
UNATTRIBUTED_LIBRARY = "(unattributed)"


class ImportTableError(Exception):
    """Malformed or out-of-policy input. Always carries a human-usable message."""


@dataclass(frozen=True)
class ImportedFunc:
    library: str
    nid: int
    stub_addr: int


@dataclass
class ImportTable:
    funcs: list[ImportedFunc] = field(default_factory=list)
    libraries: list[str] = field(default_factory=list)
    findings: list[str] = field(default_factory=list)
    # SceModuleInfo.attribute (the first u16 of the module info record). The
    # PSP_MODULE_KERNEL bit (0x1000) marks a kernel-mode module; every other
    # module runs in user mode and may import only user (syscall) libraries.
    module_attributes: int = 0


@dataclass(frozen=True)
class ExportedLibrary:
    """One SceLibraryEntryTable entry: a library and the NIDs it exports."""

    name: str | None  # None for the nameless module-lifecycle (syslib) entry
    attributes: int
    function_nids: tuple[int, ...]
    variable_nids: tuple[int, ...]


@dataclass(frozen=True)
class ExportTable:
    module_attributes: int
    libraries: tuple[ExportedLibrary, ...]

    @property
    def kernel_mode(self) -> bool:
        return bool(self.module_attributes & MODULE_ATTR_KERNEL)

    def user_callable_libraries(self) -> tuple[ExportedLibrary, ...]:
        """Libraries a user-mode importer can link against.

        Never the syslib entry (it carries the module's own lifecycle
        exports and has no library name to import by). A user-mode module's
        named libraries are all directly importable; a kernel-mode module's
        library reaches user mode only when it carries the syscall-export
        attribute, and the rest are importable by kernel code alone.
        """
        return tuple(
            library
            for library in self.libraries
            if library.name is not None
            and not library.attributes & LIB_ATTR_SYSLIB
            and (not self.kernel_mode or library.attributes & LIB_ATTR_SYSCALL_EXPORT)
        )


def _need(data: bytes, off: int, n: int, what: str) -> bytes:
    """Return data[off:off+n], refusing negative/overflowing/short reads."""
    if off < 0 or n < 0 or off + n > len(data):
        raise ImportTableError(
            f"{what}: needs bytes [{off:#x}, {off + n:#x}) but file is {len(data):#x} bytes"
        )
    return data[off : off + n]


class _GuestMap:
    """vaddr -> file offset translation built from validated load ranges."""

    def __init__(self) -> None:
        self.ranges: list[tuple[int, int, int]] = []  # (vaddr, size, file_off)

    def add(self, vaddr: int, size: int, file_off: int, file_size: int, what: str) -> None:
        if size == 0:
            return
        if vaddr > U32_MAX or size > U32_MAX or vaddr + size > U32_MAX + 1:
            raise ImportTableError(f"{what}: guest range {vaddr:#x}+{size:#x} wraps the 32-bit space")
        if file_off + size > file_size:
            raise ImportTableError(
                f"{what}: file range {file_off:#x}+{size:#x} exceeds file size {file_size:#x}"
            )
        self.ranges.append((vaddr, size, file_off))

    def to_off(self, vaddr: int, n: int, what: str) -> int:
        if vaddr > U32_MAX or n < 0 or vaddr + n > U32_MAX + 1:
            raise ImportTableError(f"{what}: guest address {vaddr:#x}+{n:#x} wraps the 32-bit space")
        for base, size, file_off in self.ranges:
            if base <= vaddr and vaddr + n <= base + size:
                return file_off + (vaddr - base)
        raise ImportTableError(f"{what}: guest address {vaddr:#x}+{n:#x} is not in any loaded range")


def _read_guest(data: bytes, gmap: _GuestMap, vaddr: int, n: int, what: str) -> bytes:
    return _need(data, gmap.to_off(vaddr, n, what), n, what)


def _read_guest_cstr(data: bytes, gmap: _GuestMap, vaddr: int, what: str) -> str:
    out = bytearray()
    for i in range(MAX_LIBNAME_LEN):
        ch = _read_guest(data, gmap, vaddr + i, 1, what)[0]
        if ch == 0:
            if not out:
                raise ImportTableError(f"{what}: empty library name at {vaddr:#x}")
            return out.decode("latin1")
        if ch < 0x20 or ch > 0x7E:
            raise ImportTableError(f"{what}: non-printable byte {ch:#04x} in name at {vaddr:#x}")
        out.append(ch)
    raise ImportTableError(f"{what}: name at {vaddr:#x} is unterminated within {MAX_LIBNAME_LEN} bytes")


def _parse_elf_maps(data: bytes) -> tuple[_GuestMap, int | None, int | None, dict[str, tuple[int, int]]]:
    """Validate the ELF envelope.

    Returns (guest map, module-info vaddr or None, phdr[0].p_paddr or None,
    section map {name: (sh_addr, sh_size)}).
    The module-info vaddr comes from the .rodata.sceModuleInfo section when
    section headers name one; sectionless (stripped) PRX/ELF inputs instead
    locate SceModuleInfo through the PRX convention -- phdr[0].p_paddr with
    the kernel bit masked is the module info's *file offset* -- which the
    caller validates before use.
    """
    if len(data) > MAX_FILE_SIZE:
        raise ImportTableError(f"file is {len(data)} bytes; refusing inputs over {MAX_FILE_SIZE}")
    eh = _need(data, 0, 52, "ELF header")
    if eh[0:4] != b"\x7fELF":
        raise ImportTableError("not an ELF file (bad magic)")
    if eh[4] != 1 or eh[5] != 1:
        raise ImportTableError("not a 32-bit little-endian ELF (PSP requires ELFCLASS32/ELFDATA2LSB)")
    (e_machine,) = struct.unpack_from("<H", eh, 18)
    if e_machine != 8:
        raise ImportTableError(f"e_machine {e_machine} is not MIPS (8)")
    e_phoff, e_shoff = struct.unpack_from("<II", eh, 28)
    e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHHHH", eh, 42)

    gmap = _GuestMap()
    phdr0_paddr: int | None = None
    if e_phnum:
        if e_phnum > MAX_SEGMENTS:
            raise ImportTableError(f"e_phnum {e_phnum} exceeds cap {MAX_SEGMENTS}")
        if e_phentsize != 32:
            raise ImportTableError(f"e_phentsize {e_phentsize} is not 32")
        for i in range(e_phnum):
            ph = _need(data, e_phoff + i * 32, 32, f"program header {i}")
            p_type, p_offset, p_vaddr, p_paddr, p_filesz = struct.unpack_from("<IIIII", ph, 0)
            if i == 0:
                phdr0_paddr = p_paddr
            if p_type == 1:  # PT_LOAD
                gmap.add(p_vaddr, p_filesz, p_offset, len(data), f"program header {i}")

    modinfo_vaddr: int | None = None
    sections: dict[str, tuple[int, int]] = {}
    if e_shnum:
        if e_shnum > MAX_SECTIONS:
            raise ImportTableError(f"e_shnum {e_shnum} exceeds cap {MAX_SECTIONS}")
        if e_shentsize != 40:
            raise ImportTableError(f"e_shentsize {e_shentsize} is not 40")
        if e_shstrndx >= e_shnum:
            raise ImportTableError(f"e_shstrndx {e_shstrndx} out of range ({e_shnum} sections)")
        shdrs = []
        for i in range(e_shnum):
            sh = _need(data, e_shoff + i * 40, 40, f"section header {i}")
            shdrs.append(struct.unpack("<10I", sh))
        str_type, str_off, str_size = shdrs[e_shstrndx][1], shdrs[e_shstrndx][4], shdrs[e_shstrndx][5]
        if str_type != 3:  # SHT_STRTAB
            raise ImportTableError("shstrtab section is not SHT_STRTAB")
        strtab = _need(data, str_off, str_size, "section name string table")
        for i, sh in enumerate(shdrs):
            sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size = sh[0], sh[1], sh[2], sh[3], sh[4], sh[5]
            if sh_name >= len(strtab) and sh_size:
                raise ImportTableError(f"section header {i}: name offset {sh_name:#x} outside shstrtab")
            end = strtab.find(b"\0", sh_name)
            name = strtab[sh_name : end if end >= 0 else len(strtab)]
            if name:
                sections[name] = (sh_addr, sh_size)
            if name == MODULE_INFO_SECTION:
                if sh_size < MODULE_INFO_SIZE:
                    raise ImportTableError(
                        f"{MODULE_INFO_SECTION.decode()} is {sh_size} bytes; need {MODULE_INFO_SIZE}"
                    )
                modinfo_vaddr = sh_addr
            # SHT_PROGBITS with SHF_ALLOC backs guest ranges when there are no
            # program headers (relocatable PRX fixtures / stripped inputs).
            if not e_phnum and sh_type == 1 and (sh_flags & 2):
                gmap.add(sh_addr, sh_size, sh_offset, len(data), f"section header {i}")

    if not gmap.ranges:
        raise ImportTableError("no PT_LOAD segments or allocatable PROGBITS sections to map guest memory")
    return gmap, modinfo_vaddr, phdr0_paddr, sections


def _find_module_info(
    data: bytes, gmap: _GuestMap, modinfo_vaddr: int | None, phdr0_paddr: int | None
) -> bytes | None:
    """Return the 52-byte SceModuleInfo record, or None when the input declares none.

    Sectioned inputs name it via .rodata.sceModuleInfo. Stripped PRX/ELF
    inputs use the PRX loader convention instead: phdr[0].p_paddr with the
    kernel-mode bit (bit 31) masked off is the record's file offset. The
    offset must land inside a mapped load range so a forged p_paddr cannot
    reach arbitrary file bytes outside guest-visible data. An input with
    neither a module-info section nor a nonzero phdr[0].p_paddr declares no
    record (None); a declared record that cannot be read is an error.
    """
    if modinfo_vaddr is not None:
        return _read_guest(data, gmap, modinfo_vaddr, MODULE_INFO_SIZE, "SceModuleInfo")
    if not phdr0_paddr:
        return None
    file_off = phdr0_paddr & 0x7FFFFFFF
    for _base, size, range_off in gmap.ranges:
        if range_off <= file_off and file_off + MODULE_INFO_SIZE <= range_off + size:
            return _need(data, file_off, MODULE_INFO_SIZE, "sectionless SceModuleInfo")
    raise ImportTableError(
        f"sectionless SceModuleInfo file offset {file_off:#x} (from phdr[0].p_paddr "
        f"{phdr0_paddr:#x}) is not inside any loaded file range"
    )


def _locate_module_info(data: bytes, gmap: _GuestMap, modinfo_vaddr: int | None, phdr0_paddr: int | None) -> bytes:
    """Return the 52-byte SceModuleInfo record; an input without one is an error."""
    record = _find_module_info(data, gmap, modinfo_vaddr, phdr0_paddr)
    if record is None:
        raise ImportTableError(
            f"no {MODULE_INFO_SECTION.decode()} section and phdr[0].p_paddr is absent/zero; "
            "cannot locate SceModuleInfo"
        )
    return record


def parse_export_table(data: bytes) -> ExportTable | None:
    """Parse the module attributes and every exported library out of a PSP ELF.

    Returns None when the input declares no SceModuleInfo record at all (see
    _find_module_info): such an image exports nothing and carries no
    kernel-mode attribute. Every declared table is read in full or rejected
    with ImportTableError, including each entry's guest address table, which
    the loader reads alongside the NIDs.
    """
    gmap, modinfo_vaddr, phdr0_paddr, _sections = _parse_elf_maps(data)
    mi = _find_module_info(data, gmap, modinfo_vaddr, phdr0_paddr)
    if mi is None:
        return None
    (module_attributes,) = struct.unpack_from("<H", mi, 0)
    ent_top, ent_end = struct.unpack_from("<II", mi, 36)
    if ent_top > ent_end:
        raise ImportTableError(f"ent_top {ent_top:#x} is above ent_end {ent_end:#x}")
    if ent_end - ent_top > MAX_EXPORT_TABLE_BYTES:
        raise ImportTableError(
            f"export table spans {ent_end - ent_top:#x} bytes; exceeds the "
            f"{MAX_EXPORT_TABLE_BYTES:#x}-byte loader bound"
        )

    libraries: list[ExportedLibrary] = []
    pos = ent_top
    while pos < ent_end:
        what = f"export entry {len(libraries)} at {pos:#x}"
        if ent_end - pos < EXPORT_ENTRY_HEADER_SIZE:
            raise ImportTableError(
                f"{what}: truncated ({ent_end - pos} bytes left, need {EXPORT_ENTRY_HEADER_SIZE})"
            )
        header = _read_guest(data, gmap, pos, EXPORT_ENTRY_HEADER_SIZE, what)
        name_ptr, _version, attributes, size_words, num_vars, num_funcs, entry_table = struct.unpack(
            "<IHHBBHI", header
        )
        if size_words < MIN_EXPORT_ENTRY_WORDS or size_words > MAX_EXPORT_ENTRY_WORDS:
            raise ImportTableError(
                f"{what}: entry size {size_words} words outside "
                f"[{MIN_EXPORT_ENTRY_WORDS}, {MAX_EXPORT_ENTRY_WORDS}]"
            )
        next_pos = pos + size_words * 4
        if next_pos > ent_end:
            raise ImportTableError(f"{what}: entry size {size_words} words runs past ent_end {ent_end:#x}")
        if len(libraries) >= MAX_LIBRARIES:
            raise ImportTableError(f"more than {MAX_LIBRARIES} export entries")
        count = num_funcs + num_vars
        if count > MAX_FUNCS_PER_LIB:
            raise ImportTableError(f"{what}: {count} exports exceeds cap {MAX_FUNCS_PER_LIB}")
        name = _read_guest_cstr(data, gmap, name_ptr, f"{what} library name") if name_ptr else None
        nids: tuple[int, ...] = ()
        if count:
            if entry_table == 0:
                raise ImportTableError(f"{what}: {count} exports but null entry table pointer")
            nid_blob = _read_guest(data, gmap, entry_table, count * 4, f"{what} NID table")
            _read_guest(data, gmap, entry_table + count * 4, count * 4, f"{what} address table")
            nids = struct.unpack_from(f"<{count}I", nid_blob)
        libraries.append(ExportedLibrary(name, attributes, nids[:num_funcs], nids[num_funcs:]))
        pos = next_pos
    return ExportTable(module_attributes, tuple(libraries))


def parse_import_table(data: bytes) -> ImportTable:
    """Parse (library, NID, stub address) triples out of a PSP ELF byte buffer."""
    gmap, modinfo_vaddr, phdr0_paddr, sections = _parse_elf_maps(data)
    mi = _locate_module_info(data, gmap, modinfo_vaddr, phdr0_paddr)
    libstub, libstubend = struct.unpack_from("<II", mi, 44)
    if libstub > libstubend:
        raise ImportTableError(f"libstub {libstub:#x} is above libstubend {libstubend:#x}")
    if libstubend - libstub > MAX_LIBRARIES * MAX_STUB_ENTRY_WORDS * 4:
        raise ImportTableError(
            f"libstub table spans {libstubend - libstub:#x} bytes; exceeds defensive cap"
        )

    table = ImportTable(module_attributes=struct.unpack_from("<H", mi, 0)[0])
    seen_libs: set[str] = set()
    windows = []  # (library name, numFuncs, nidData, firstSymAddr)
    pos = libstub
    entry_index = 0
    while pos < libstubend:
        what = f"import entry {entry_index} at {pos:#x}"
        if libstubend - pos < STUB_ENTRY_HEADER_SIZE:
            raise ImportTableError(f"{what}: truncated ({libstubend - pos} bytes left, need {STUB_ENTRY_HEADER_SIZE})")
        e = _read_guest(data, gmap, pos, STUB_ENTRY_HEADER_SIZE, what)
        name_ptr, _ver, _flags, size_words, num_vars, num_funcs, nid_data, first_sym = struct.unpack(
            "<IHHBBHII", e
        )
        if size_words < MIN_STUB_ENTRY_WORDS or size_words > MAX_STUB_ENTRY_WORDS:
            raise ImportTableError(
                f"{what}: entry size {size_words} words outside "
                f"[{MIN_STUB_ENTRY_WORDS}, {MAX_STUB_ENTRY_WORDS}]"
            )
        next_pos = pos + size_words * 4
        if next_pos > libstubend:
            raise ImportTableError(f"{what}: entry size {size_words} words runs past libstubend {libstubend:#x}")
        if name_ptr == 0:
            raise ImportTableError(f"{what}: null library name pointer")
        libname = _read_guest_cstr(data, gmap, name_ptr, f"{what} library name")
        # Duplicate library entries are tolerated (some linkers emit split
        # blocks for one library); duplicate NIDs are surfaced by the auditor.
        seen_libs.add(libname)
        if len(seen_libs) > MAX_LIBRARIES:
            raise ImportTableError(f"more than {MAX_LIBRARIES} import libraries")
        if num_funcs > MAX_FUNCS_PER_LIB:
            raise ImportTableError(f"{what}: {num_funcs} functions exceeds cap {MAX_FUNCS_PER_LIB}")
        if num_funcs and nid_data == 0:
            raise ImportTableError(f"{what}: {num_funcs} functions but null NID table pointer")
        _ = num_vars  # variable imports carry no NIDs we audit; presence is fine
        table.libraries.append(libname)
        if num_funcs:
            # Per-window pointer validation. The stub span check (first_sym ..
            # first_sym + num_funcs*8) requires 4-byte alignment, no 32-bit
            # wrap, and one fully file-backed mapped range (stubs are code; a
            # partially mapped span means a truncated or forged table).
            _read_guest(data, gmap, nid_data, num_funcs * 4, f"{what} NID table")
            if first_sym % 4:
                raise ImportTableError(f"{what}: stub area {first_sym:#x} is not 4-byte aligned")
            if first_sym + num_funcs * 8 > U32_MAX + 1:
                raise ImportTableError(f"{what}: stub area {first_sym:#x} wraps the 32-bit space")
            gmap.to_off(first_sym, num_funcs * 8, f"{what} function stub span")
        windows.append((libname, num_funcs, nid_data, first_sym))
        pos = next_pos
        entry_index += 1
    if not windows:
        return table

    # Full stub/NID region extents. The pairing regions are .sceStub.text and
    # .rodata.sceNid when section headers name them; stripped inputs fall back
    # to the union of the window runs (which is the loader's view: it can only
    # patch positions some window claims).
    st = sections.get(STUB_SECTION)
    ns = sections.get(NID_SECTION)
    stub_base = stub_end = None
    nid_base = nid_end = None
    if st is not None:
        addr, size = st
        if size % 8:
            raise ImportTableError(
                f"{STUB_SECTION.decode()} size {size:#x} is not a multiple of 8 (stub slots are 8 bytes)")
        stub_base, stub_end = addr, addr + size
    if ns is not None:
        addr, size = ns
        if size % 4:
            raise ImportTableError(f"{NID_SECTION.decode()} size {size:#x} is not a multiple of 4")
        nid_base, nid_end = addr, addr + size
    if stub_base is None:
        stub_base = min(w[3] for w in windows)
        stub_end = max(w[3] + w[1] * 8 for w in windows)
    if nid_base is None:
        func_windows = [w for w in windows if w[1]]
        if func_windows:
            nid_base = min(w[2] for w in func_windows)
            nid_end = max(w[2] + w[1] * 4 for w in func_windows)
    if stub_end - stub_base != 2 * (nid_end - nid_base):
        raise ImportTableError(
            "import stub region size does not match NID region size "
            "(psp-fixup-imports requires stub slots to pair 1:1 with NIDs)"
        )
    stub_count = (stub_end - stub_base) // 8
    nid_count = (nid_end - nid_base) // 4
    if stub_count != nid_count or nid_count <= 0:
        raise ImportTableError(f"impossible import region: {stub_count} stub slots vs {nid_count} NIDs")
    if nid_count > MAX_TOTAL_FUNCS:
        raise ImportTableError(f"more than {MAX_TOTAL_FUNCS} imported functions")
    nid_blob = _read_guest(data, gmap, nid_base, nid_count * 4, "global NID region")
    nids = struct.unpack_from(f"<{nid_count}I", nid_blob)

    # Lay window claims over global positions; the global pairing fixes each
    # position's NID, so overlapping claims agree and the last claimer owns
    # the library attribution (the toolchain emits each library's run from
    # its first slot, so the last window to reach a position is its owner).
    claims: dict[int, str] = {}
    claimers: dict[int, list[str]] = {}
    for libname, num_funcs, nid_data, first_sym in windows:
        if num_funcs == 0:
            continue
        if first_sym < stub_base or (first_sym - stub_base) % 8:
            raise ImportTableError(
                f"import stub address 0x{first_sym:08x} is not an 8-byte slot of the stub region")
        if nid_data < nid_base or (nid_data - nid_base) % 4:
            raise ImportTableError(
                f"import NID table 0x{nid_data:08x} is not a 4-byte slot of the NID region")
        first_pos = (first_sym - stub_base) // 8
        nid_pos = (nid_data - nid_base) // 4
        if first_pos != nid_pos:
            raise ImportTableError(
                f"inconsistent import window {libname}: stub slot {first_pos} "
                f"but NID slot {nid_pos}")
        if first_pos + num_funcs > stub_count:
            raise ImportTableError(
                f"import window {libname} with {num_funcs} functions runs past "
                f"the stub region ({stub_count} slots)")
        for i in range(num_funcs):
            p = first_pos + i
            claims[p] = libname
            claimers.setdefault(p, []).append(libname)

    for p in range(nid_count):
        table.funcs.append(
            ImportedFunc(claims.get(p, UNATTRIBUTED_LIBRARY), nids[p], stub_base + p * 8)
        )

    unclaimed = [p for p in range(nid_count) if p not in claims]
    if unclaimed:
        table.findings.append(
            f"stub slots not covered by any library window: {len(unclaimed)} positions {unclaimed}"
        )
    ambiguous = sorted(p for p, libs in claimers.items() if len(libs) > 1)
    if ambiguous:
        table.findings.append(
            f"stub slots claimed by multiple library windows: {len(ambiguous)} positions {ambiguous}"
        )
    return table
